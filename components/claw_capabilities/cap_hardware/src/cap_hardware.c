/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_hardware.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "cap_hardware";

/* Matches the largest existing JSON-ish config field in this project (ssh_authorized_public_key,
 * app_claw.h) — enough for roughly a dozen pin entries, which is already a generous board. */
#define CAP_HARDWARE_PINS_JSON_MAX 1024

typedef struct {
    char pins_json[CAP_HARDWARE_PINS_JSON_MAX];
} cap_hardware_state_t;

static cap_hardware_state_t s_hardware = {0};
/* Lazily created; ADC1 only (ADC2 shares hardware with Wi-Fi and is unreliable while it is active,
 * and this firmware always runs Wi-Fi — see the header's design note). */
static adc_oneshot_unit_handle_t s_adc1_unit;

esp_err_t cap_hardware_set_pins(const char *pins_json)
{
    if (!pins_json) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_hardware.pins_json, pins_json, sizeof(s_hardware.pins_json));
    return ESP_OK;
}

/*
 * Pins that must never be repurposed on the compiled target, regardless of what an operator
 * configures: the SPI flash/PSRAM bus. Writing to these can corrupt flash or crash the device
 * immediately and deterministically, unlike a merely inconvenient pin choice. This is intentionally
 * narrow (see cap_hardware.h) — it is a safety net under the operator's own allow-list, not a
 * replacement for reviewing it. UART/strapping pins beyond the well-known ESP32 console pair are
 * left to the operator: whether they are safe to repurpose is board-specific (e.g. whether the
 * console is on UART0 or USB-CDC), and this file has no reliable way to know that without risking a
 * wrong claim.
 */
static bool cap_hardware_pin_hard_denied(int gpio)
{
#if CONFIG_IDF_TARGET_ESP32
    /* GPIO6-11: SPI flash. GPIO1/3: default UART0 console TX/RX on this target. */
    return (gpio >= 6 && gpio <= 11) || gpio == 1 || gpio == 3;
#elif CONFIG_IDF_TARGET_ESP32S3
    /* GPIO26-32: SPI flash / octal PSRAM on ESP32-S3 modules. */
    return gpio >= 26 && gpio <= 32;
#else
    (void)gpio;
    return false; /* Unknown target: no verified deny-list — rely on the operator's own review. */
#endif
}

static const char *cap_hardware_json_str(const cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/*
 * Looks up `name`, validates it against `need_type`/`need_mode` (NULL = don't care) and, if
 * `require_allowed`, that "allowed":true is set, then checks the hard deny-list. On success returns
 * the resolved GPIO number and the matching entry's "type"/"mode" strings (heap copies the caller
 * must free with free() — never cJSON_free(), since they are produced by strdup(), not cJSON; see
 * the project's own strdup()-vs-cJSON_free() mismatch rule). On failure, `output` already contains a
 * complete error message for the caller to return as-is.
 */
static esp_err_t cap_hardware_resolve(const char *name, const char *need_type, const char *need_mode,
                                      bool require_allowed, int *out_gpio, char **out_type,
                                      char **out_mode, char *output, size_t output_size)
{
    cJSON *root = NULL;
    cJSON *entry = NULL;
    cJSON *found = NULL;
    const char *type = NULL;
    const char *mode = NULL;
    cJSON *gpio_item = NULL;
    cJSON *allowed_item = NULL;
    int gpio;

    *out_type = NULL;
    *out_mode = NULL;

    if (!s_hardware.pins_json[0]) {
        snprintf(output, output_size, "Error: no hardware is configured on this device");
        return ESP_ERR_NOT_FOUND;
    }
    root = cJSON_Parse(s_hardware.pins_json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: hw_pins configuration is invalid; ask the device owner to fix it");
        return ESP_ERR_INVALID_STATE;
    }
    cJSON_ArrayForEach(entry, root) {
        const char *entry_name = cap_hardware_json_str(entry, "name");
        if (entry_name && strcmp(entry_name, name) == 0) {
            found = entry;
            break;
        }
    }
    if (!found) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: no configured hardware named '%s'", name);
        return ESP_ERR_NOT_FOUND;
    }

    type = cap_hardware_json_str(found, "type");
    mode = cap_hardware_json_str(found, "mode");
    gpio_item = cJSON_GetObjectItem(found, "gpio");
    allowed_item = cJSON_GetObjectItem(found, "allowed");

    if (!cJSON_IsNumber(gpio_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: '%s' has no valid gpio configured", name);
        return ESP_ERR_INVALID_STATE;
    }
    gpio = gpio_item->valueint;

    if (need_type && (!type || strcmp(type, need_type) != 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: '%s' is not configured as a %s", name, need_type);
        return ESP_ERR_INVALID_STATE;
    }
    if (need_mode && (!mode || strcmp(mode, need_mode) != 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: '%s' is not configured for %s mode", name, need_mode);
        return ESP_ERR_INVALID_STATE;
    }
    if (require_allowed && !(cJSON_IsBool(allowed_item) && cJSON_IsTrue(allowed_item))) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: '%s' is not allowed to be written (allowed=false)", name);
        return ESP_ERR_INVALID_STATE;
    }
    if (cap_hardware_pin_hard_denied(gpio)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Refusing to touch GPIO %d (%s): reserved for flash/PSRAM/console on this target", gpio, name);
        snprintf(output, output_size, "Error: GPIO %d is reserved on this board and cannot be controlled", gpio);
        return ESP_ERR_INVALID_STATE;
    }

    *out_gpio = gpio;
    *out_type = type ? strdup(type) : NULL;
    *out_mode = mode ? strdup(mode) : NULL;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_hardware_gpio_read_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *name = NULL;
    int gpio = -1;
    char *type = NULL;
    char *mode = NULL;
    esp_err_t err;

    (void)ctx;
    input = cJSON_Parse(input_json);
    name = input ? cap_hardware_json_str(input, "name") : NULL;
    if (!name) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'name' is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Any configured digital device may be read back (including an output/switch's driven level). */
    err = cap_hardware_resolve(name, NULL, NULL, false, &gpio, &type, &mode, output, output_size);
    cJSON_Delete(input);
    if (err != ESP_OK) {
        return err;
    }
    free(type);
    if (mode && strcmp(mode, "analog") == 0) {
        free(mode);
        snprintf(output, output_size, "Error: '%s' is an analog sensor; use sensor_read", name);
        return ESP_ERR_INVALID_ARG;
    }
    free(mode);

    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT);
    int level = gpio_get_level((gpio_num_t)gpio);
    snprintf(output, output_size, "{\"name\":\"%s\",\"gpio\":%d,\"level\":%s}", name, gpio, level ? "true" : "false");
    return ESP_OK;
}

static esp_err_t cap_hardware_gpio_write_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output, size_t output_size)
{
    cJSON *input = NULL;
    cJSON *value_item = NULL;
    const char *name = NULL;
    int gpio = -1;
    char *type = NULL;
    char *mode = NULL;
    bool value;
    esp_err_t err;

    (void)ctx;
    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }
    name = cap_hardware_json_str(input, "name");
    value_item = cJSON_GetObjectItem(input, "value");
    if (!name || !cJSON_IsBool(value_item)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'name' (string) and 'value' (boolean) are required");
        return ESP_ERR_INVALID_ARG;
    }
    value = cJSON_IsTrue(value_item);
    cJSON_Delete(input);

    /* Requires mode="output" and allowed=true; type is checked explicitly below (switch or output). */
    err = cap_hardware_resolve(name, NULL, "output", true, &gpio, &type, &mode, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    free(mode);
    if (!type || (strcmp(type, "switch") != 0 && strcmp(type, "output") != 0)) {
        free(type);
        snprintf(output, output_size, "Error: '%s' is not a switch/output", name);
        return ESP_ERR_INVALID_STATE;
    }
    free(type);

    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)gpio, value ? 1 : 0);
    ESP_LOGI(TAG, "gpio_write: %s (GPIO %d) -> %s", name, gpio, value ? "ON" : "OFF");

    snprintf(output, output_size, "{\"ok\":true,\"name\":\"%s\",\"gpio\":%d,\"value\":%s}",
             name, gpio, value ? "true" : "false");
    return ESP_OK;
}

static esp_err_t cap_hardware_ensure_adc1(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };

    if (s_adc1_unit) {
        return ESP_OK;
    }
    return adc_oneshot_new_unit(&init_cfg, &s_adc1_unit);
}

static esp_err_t cap_hardware_sensor_read_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output, size_t output_size)
{
    cJSON *input = NULL;
    const char *name = NULL;
    int gpio = -1;
    char *type = NULL;
    char *mode = NULL;
    esp_err_t err;

    (void)ctx;
    input = cJSON_Parse(input_json);
    name = input ? cap_hardware_json_str(input, "name") : NULL;
    if (!name) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'name' is required");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_hardware_resolve(name, "sensor", NULL, false, &gpio, &type, &mode, output, output_size);
    cJSON_Delete(input);
    if (err != ESP_OK) {
        return err;
    }
    free(type);

    if (mode && strcmp(mode, "analog") == 0) {
        adc_unit_t unit;
        adc_channel_t channel;
        adc_oneshot_chan_cfg_t chan_cfg = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
        int raw = 0;

        free(mode);
        if (adc_oneshot_io_to_channel(gpio, &unit, &channel) != ESP_OK) {
            snprintf(output, output_size, "Error: GPIO %d has no ADC channel on this target", gpio);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (unit != ADC_UNIT_1) {
            snprintf(output, output_size,
                     "Error: GPIO %d is on ADC2, which is unreliable while Wi-Fi is active; not supported", gpio);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (cap_hardware_ensure_adc1() != ESP_OK ||
                adc_oneshot_config_channel(s_adc1_unit, channel, &chan_cfg) != ESP_OK ||
                adc_oneshot_read(s_adc1_unit, channel, &raw) != ESP_OK) {
            snprintf(output, output_size, "Error: failed to read GPIO %d as analog", gpio);
            return ESP_FAIL;
        }
        snprintf(output, output_size, "{\"name\":\"%s\",\"gpio\":%d,\"raw\":%d}", name, gpio, raw);
        return ESP_OK;
    }

    free(mode);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT);
    int level = gpio_get_level((gpio_num_t)gpio);
    snprintf(output, output_size, "{\"name\":\"%s\",\"gpio\":%d,\"level\":%s}", name, gpio, level ? "true" : "false");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_hardware_descriptors[] = {
    {
        .id = "gpio_read",
        .name = "gpio_read",
        .family = "hardware",
        .description = "Read the current digital level of a configured hardware device by name "
                       "(e.g. is a switch currently on). Use sensor_read for analog sensors.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
        .execute = cap_hardware_gpio_read_execute,
    },
    {
        .id = "gpio_write",
        .name = "gpio_write",
        .family = "hardware",
        .description = "Turn a configured switch/output device on or off by name. Only works for "
                       "devices the operator marked mode=\"output\" and allowed=true in the "
                       "hardware configuration.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
        "\"value\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"value\"]}",
        .execute = cap_hardware_gpio_write_execute,
    },
    {
        .id = "sensor_read",
        .name = "sensor_read",
        .family = "hardware",
        .description = "Read a configured sensor device by name. Returns {\"raw\":0-4095} for an "
                       "analog sensor (mode=\"analog\") or {\"level\":bool} for a digital one.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
        .execute = cap_hardware_sensor_read_execute,
    },
};

static const claw_cap_group_t s_hardware_group = {
    .group_id = "cap_hardware",
    .descriptors = s_hardware_descriptors,
    .descriptor_count = sizeof(s_hardware_descriptors) / sizeof(s_hardware_descriptors[0]),
};

esp_err_t cap_hardware_register_group(void)
{
    if (claw_cap_group_exists(s_hardware_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_hardware_group);
}
