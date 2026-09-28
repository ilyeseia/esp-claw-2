/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_home_assistant.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "cap_home_assistant";

#define CAP_HA_BASE_URL_MAX     320  /* matches APP_CLAW_STR_LEN */
#define CAP_HA_TOKEN_MAX        320  /* matches APP_CLAW_STR_LEN */
#define CAP_HA_IDENTIFIER_MAX   96   /* generous for a Home Assistant entity_id/domain/service */
#define CAP_HA_TIMEOUT_MS       8000
#define CAP_HA_MAX_RESPONSE     (48 * 1024)  /* /api/states on a large install can be sizeable */
#define CAP_HA_MAX_ENTITIES_OUT 40            /* cap what actually reaches the LLM's context */

typedef struct {
    bool enabled;
    char base_url[CAP_HA_BASE_URL_MAX];
    char token[CAP_HA_TOKEN_MAX];
} cap_ha_state_t;

static cap_ha_state_t s_ha = {0};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_ha_buf_t;

esp_err_t cap_home_assistant_set_config(bool enabled, const char *base_url, const char *token)
{
    s_ha.enabled = enabled;
    strlcpy(s_ha.base_url, base_url ? base_url : "", sizeof(s_ha.base_url));
    strlcpy(s_ha.token, token ? token : "", sizeof(s_ha.token));
    return ESP_OK;
}

static const char *cap_ha_json_str(const cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/*
 * Home Assistant identifiers (entity_id, domain, service) are always lowercase ASCII, digits and
 * underscores, with entity_id additionally containing exactly one '.' separating domain and object
 * id (https://developers.home-assistant.io/docs/core/entity/#entity-id). Rejecting anything outside
 * that charset before it is placed into a URL path means the model can never smuggle a '/', '?',
 * '#', or whitespace into the request — it can only ever address a real-shaped HA identifier, never
 * redirect the request to a different path or host.
 */
static bool cap_ha_identifier_valid(const char *value, bool allow_one_dot)
{
    bool seen_dot = false;
    size_t len;

    if (!value || !value[0]) {
        return false;
    }
    len = strlen(value);
    if (len >= CAP_HA_IDENTIFIER_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (islower(c) || isdigit(c) || c == '_') {
            continue;
        }
        if (allow_one_dot && c == '.' && !seen_dot) {
            seen_dot = true;
            continue;
        }
        return false;
    }
    return true;
}

static esp_err_t cap_ha_event_handler(esp_http_client_event_t *event)
{
    cap_ha_buf_t *buf = NULL;

    if (!event || event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    buf = (cap_ha_buf_t *)event->user_data;
    if (!buf || !buf->data || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t append_len = (size_t)event->data_len;
    if (buf->len + append_len + 1 > buf->cap) {
        /* Bounded: never grow past CAP_HA_MAX_RESPONSE. Anything beyond that is dropped, and the
         * eventual JSON parse of a truncated body will simply fail with a clear error below. */
        if (buf->cap >= CAP_HA_MAX_RESPONSE) {
            return ESP_OK;
        }
        size_t new_cap = buf->cap * 2;
        if (new_cap > CAP_HA_MAX_RESPONSE) {
            new_cap = CAP_HA_MAX_RESPONSE;
        }
        if (new_cap < buf->len + append_len + 1) {
            append_len = (new_cap > buf->len + 1) ? (new_cap - buf->len - 1) : 0;
        }
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    if (append_len > 0) {
        memcpy(buf->data + buf->len, event->data, append_len);
        buf->len += append_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

/*
 * Performs one request against {base_url}{path}. `body` (may be NULL) is sent as-is with
 * Content-Type: application/json for a POST; NULL/empty means GET. The Authorization header value
 * is set on the client but never logged, only the path and the resulting status code are. Returns
 * the response body (caller frees with free()) and the HTTP status via *out_status, or an esp_err_t
 * describing a transport failure (never a hang: CAP_HA_TIMEOUT_MS bounds it).
 */
static esp_err_t cap_ha_request(const char *path, const char *body, char **out_body, int *out_status)
{
    char url[CAP_HA_BASE_URL_MAX + CAP_HA_IDENTIFIER_MAX * 2 + 32];
    cap_ha_buf_t buf = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    *out_body = NULL;
    *out_status = 0;

    if (snprintf(url, sizeof(url), "%s%s", s_ha.base_url, path) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buf.cap = 4096;
    buf.data = calloc(1, buf.cap);
    if (!buf.data) {
        return ESP_ERR_NO_MEM;
    }

    config.url = url;
    config.event_handler = cap_ha_event_handler;
    config.user_data = &buf;
    config.timeout_ms = CAP_HA_TIMEOUT_MS;
    config.buffer_size = 2048;
    /* Real certificate validation whenever https:// is used (never bypassed, unlike the reference
     * implementation's unconditional setInsecure() — see cap_home_assistant.h). Home Assistant on a
     * home LAN is very commonly plain http://, which esp_http_client handles unmodified: this
     * option only takes effect for an https:// URL. */
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        free(buf.data);
        return ESP_FAIL;
    }
    if (s_ha.token[0]) {
        char auth[CAP_HA_TOKEN_MAX + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", s_ha.token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    ESP_LOGI(TAG, "HA request: %s %s", body ? "POST" : "GET", path);
    err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HA response: %s -> HTTP %d", path, *out_status);

    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }
    *out_body = buf.data;
    return ESP_OK;
}

static bool cap_ha_not_configured(char *output, size_t output_size)
{
    if (!s_ha.enabled) {
        snprintf(output, output_size, "Error: Home Assistant integration is disabled");
        return true;
    }
    if (!s_ha.base_url[0] || !s_ha.token[0]) {
        snprintf(output, output_size, "Error: Home Assistant is not configured (base URL/token missing)");
        return true;
    }
    return false;
}

/* Shared by ha_get_entities and ha_search_entity: fetches /api/states and returns the parsed array
 * (caller must cJSON_Delete it), or NULL with `output` already filled in on failure. */
static cJSON *cap_ha_fetch_states(char *output, size_t output_size)
{
    char *body = NULL;
    int status = 0;
    esp_err_t err = cap_ha_request("/api/states", NULL, &body, &status);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: could not reach Home Assistant (%s)", esp_err_to_name(err));
        return NULL;
    }
    if (status != 200) {
        snprintf(output, output_size, "Error: Home Assistant returned HTTP %d", status);
        free(body);
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: unexpected response from Home Assistant");
        return NULL;
    }
    return root;
}

/* Appends a trimmed {entity_id, state, name} for `entity` to `out`. */
static void cap_ha_append_trimmed(cJSON *out, cJSON *entity)
{
    const char *entity_id = cap_ha_json_str(entity, "entity_id");
    const char *state = cap_ha_json_str(entity, "state");
    cJSON *attributes = cJSON_GetObjectItem(entity, "attributes");
    const char *friendly_name = attributes ? cap_ha_json_str(attributes, "friendly_name") : NULL;
    cJSON *item = cJSON_CreateObject();

    cJSON_AddStringToObject(item, "entity_id", entity_id ? entity_id : "");
    cJSON_AddStringToObject(item, "state", state ? state : "unknown");
    cJSON_AddStringToObject(item, "name", friendly_name ? friendly_name : (entity_id ? entity_id : ""));
    cJSON_AddItemToArray(out, item);
}

static esp_err_t cap_ha_get_entities_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *domain = NULL;
    cJSON *states = NULL;
    cJSON *result = NULL;
    cJSON *entity = NULL;
    int included = 0;
    int total = 0;

    (void)ctx;
    if (cap_ha_not_configured(output, output_size)) {
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    domain = input ? cap_ha_json_str(input, "domain") : NULL;
    if (domain && !cap_ha_identifier_valid(domain, false)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'domain' must look like a Home Assistant domain, e.g. \"light\"");
        return ESP_ERR_INVALID_ARG;
    }

    states = cap_ha_fetch_states(output, output_size);
    if (!states) {
        cJSON_Delete(input);
        return ESP_FAIL;
    }

    result = cJSON_CreateArray();
    cJSON_ArrayForEach(entity, states) {
        const char *entity_id = cap_ha_json_str(entity, "entity_id");
        if (!entity_id) {
            continue;
        }
        if (domain) {
            size_t domain_len = strlen(domain);
            if (strncmp(entity_id, domain, domain_len) != 0 || entity_id[domain_len] != '.') {
                continue;
            }
        }
        total++;
        if (included < CAP_HA_MAX_ENTITIES_OUT) {
            cap_ha_append_trimmed(result, entity);
            included++;
        }
    }
    cJSON_Delete(states);
    cJSON_Delete(input);

    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddNumberToObject(envelope, "total_matching", total);
    cJSON_AddBoolToObject(envelope, "truncated", total > included);
    cJSON_AddItemToObject(envelope, "entities", result);
    char *text = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!text) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static esp_err_t cap_ha_search_entity_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *query = NULL;
    char query_lower[CAP_HA_IDENTIFIER_MAX * 2] = {0};
    cJSON *states = NULL;
    cJSON *result = NULL;
    cJSON *entity = NULL;
    int included = 0;

    (void)ctx;
    if (cap_ha_not_configured(output, output_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    input = cJSON_Parse(input_json);
    query = input ? cap_ha_json_str(input, "query") : NULL;
    if (!query || !query[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'query' is required");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(query_lower, query, sizeof(query_lower));
    for (char *p = query_lower; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    states = cap_ha_fetch_states(output, output_size);
    if (!states) {
        cJSON_Delete(input);
        return ESP_FAIL;
    }

    result = cJSON_CreateArray();
    cJSON_ArrayForEach(entity, states) {
        if (included >= CAP_HA_MAX_ENTITIES_OUT) {
            break;
        }
        const char *entity_id = cap_ha_json_str(entity, "entity_id");
        cJSON *attributes = cJSON_GetObjectItem(entity, "attributes");
        const char *friendly_name = attributes ? cap_ha_json_str(attributes, "friendly_name") : NULL;
        bool matches = false;

        if (entity_id && strcasestr(entity_id, query_lower)) {
            matches = true;
        }
        if (!matches && friendly_name && strcasestr(friendly_name, query_lower)) {
            matches = true;
        }
        if (matches) {
            cap_ha_append_trimmed(result, entity);
            included++;
        }
    }
    cJSON_Delete(states);
    cJSON_Delete(input);

    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!text) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static esp_err_t cap_ha_get_state_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *entity_id = NULL;
    char path[CAP_HA_IDENTIFIER_MAX + 32];
    char *body = NULL;
    int status = 0;
    esp_err_t err;

    (void)ctx;
    if (cap_ha_not_configured(output, output_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    input = cJSON_Parse(input_json);
    entity_id = input ? cap_ha_json_str(input, "entity_id") : NULL;
    if (!entity_id || !cap_ha_identifier_valid(entity_id, true)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'entity_id' must look like \"domain.object_id\"");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(path, sizeof(path), "/api/states/%s", entity_id);
    cJSON_Delete(input);

    err = cap_ha_request(path, NULL, &body, &status);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: could not reach Home Assistant (%s)", esp_err_to_name(err));
        return err;
    }
    if (status == 404) {
        free(body);
        snprintf(output, output_size, "Error: no such entity '%s'", entity_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (status != 200) {
        free(body);
        snprintf(output, output_size, "Error: Home Assistant returned HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON *entity = cJSON_Parse(body);
    free(body);
    if (!entity) {
        snprintf(output, output_size, "Error: unexpected response from Home Assistant");
        return ESP_FAIL;
    }
    const char *state = cap_ha_json_str(entity, "state");
    cJSON *attributes = cJSON_GetObjectItem(entity, "attributes");
    const char *friendly_name = attributes ? cap_ha_json_str(attributes, "friendly_name") : NULL;
    const char *unit = attributes ? cap_ha_json_str(attributes, "unit_of_measurement") : NULL;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "entity_id", entity_id);
    cJSON_AddStringToObject(result, "state", state ? state : "unknown");
    cJSON_AddStringToObject(result, "name", friendly_name ? friendly_name : entity_id);
    if (unit) {
        cJSON_AddStringToObject(result, "unit_of_measurement", unit);
    }
    cJSON_Delete(entity);

    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!text) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static esp_err_t cap_ha_call_service_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *domain = NULL;
    const char *service = NULL;
    cJSON *data_item = NULL;
    char path[CAP_HA_IDENTIFIER_MAX * 2 + 32];
    char *body_str = NULL;
    char *response_body = NULL;
    int status = 0;
    esp_err_t err;

    (void)ctx;
    if (cap_ha_not_configured(output, output_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }
    domain = cap_ha_json_str(input, "domain");
    service = cap_ha_json_str(input, "service");
    data_item = cJSON_GetObjectItem(input, "data");
    if (!domain || !cap_ha_identifier_valid(domain, false) ||
            !service || !cap_ha_identifier_valid(service, false)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: 'domain' and 'service' must be lowercase Home Assistant identifiers, "
                 "e.g. domain=\"switch\", service=\"turn_on\"");
        return ESP_ERR_INVALID_ARG;
    }
    if (data_item && !cJSON_IsObject(data_item)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'data' must be a JSON object if provided");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
    {
        /* Always printed via cJSON's own allocator (never strdup()) so it is always freed with
         * cJSON_free() below — mixing allocators here was a real bug caught in review elsewhere in
         * this project (see the esp-claw-iot skill's C-conventions note). */
        cJSON *owned_empty = data_item ? NULL : cJSON_CreateObject();
        body_str = cJSON_PrintUnformatted(data_item ? data_item : owned_empty);
        cJSON_Delete(owned_empty);
    }
    cJSON_Delete(input);
    if (!body_str) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "HA service call: %s.%s", domain, service);
    err = cap_ha_request(path, body_str, &response_body, &status);
    cJSON_free(body_str);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: could not reach Home Assistant (%s)", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        free(response_body);
        snprintf(output, output_size, "Error: Home Assistant returned HTTP %d for %s.%s", status, domain, service);
        return ESP_FAIL;
    }
    snprintf(output, output_size, "{\"ok\":true,\"domain\":\"%s\",\"service\":\"%s\",\"status\":%d}",
             domain, service, status);
    free(response_body);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_ha_descriptors[] = {
    {
        .id = "ha_get_entities",
        .name = "ha_get_entities",
        .family = "home_assistant",
        .description = "List Home Assistant entities and their current state, optionally filtered "
                       "by domain (e.g. \"light\", \"switch\", \"sensor\"). Capped to the first "
                       "matches to keep the result small.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"domain\":{\"type\":\"string\"}}}",
        .execute = cap_ha_get_entities_execute,
    },
    {
        .id = "ha_search_entity",
        .name = "ha_search_entity",
        .family = "home_assistant",
        .description = "Search Home Assistant entities by a substring of their entity_id or "
                       "friendly name (case-insensitive).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
        .execute = cap_ha_search_entity_execute,
    },
    {
        .id = "ha_get_state",
        .name = "ha_get_state",
        .family = "home_assistant",
        .description = "Get the current state of one Home Assistant entity by its exact entity_id "
                       "(e.g. \"sensor.living_room_temperature\").",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"string\"}},\"required\":[\"entity_id\"]}",
        .execute = cap_ha_get_state_execute,
    },
    {
        .id = "ha_call_service",
        .name = "ha_call_service",
        .family = "home_assistant",
        .description = "Call a Home Assistant service, e.g. domain=\"switch\", service=\"turn_on\", "
                       "data={\"entity_id\":\"switch.living_room_fan\"}. Use ha_get_entities or "
                       "ha_search_entity first to find the right entity_id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"domain\":{\"type\":\"string\"},"
        "\"service\":{\"type\":\"string\"},\"data\":{\"type\":\"object\"}},"
        "\"required\":[\"domain\",\"service\"]}",
        .execute = cap_ha_call_service_execute,
    },
};

static const claw_cap_group_t s_ha_group = {
    .group_id = "cap_home_assistant",
    .descriptors = s_ha_descriptors,
    .descriptor_count = sizeof(s_ha_descriptors) / sizeof(s_ha_descriptors[0]),
};

esp_err_t cap_home_assistant_register_group(void)
{
    if (claw_cap_group_exists(s_ha_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_ha_group);
}
