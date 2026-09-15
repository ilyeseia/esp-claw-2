/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_ota.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_ota";

#define CAP_OTA_TASK_STACK  8192
#define CAP_OTA_TIMEOUT_MS  60000

static bool s_ota_in_progress;

static esp_err_t cap_ota_emit(cJSON *root, char *output, size_t output_size)
{
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(output, output_size, "Error: failed to encode result");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static const char *cap_ota_json_str(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

static const char *cap_ota_state_str(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

/* Runs the blocking HTTPS OTA download on its own task, then reboots on success. */
static void cap_ota_task(void *arg)
{
    char *url = (char *)arg;

    ESP_LOGW(TAG, "OTA download starting from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CAP_OTA_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA succeeded; rebooting into the new firmware");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    s_ota_in_progress = false;
    vTaskDelete(NULL);
}

static esp_err_t cap_ota_update_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)ctx;

    if (s_ota_in_progress) {
        snprintf(output, output_size, "Error: an OTA update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * SECURITY: https:// only. esp_https_ota() has no independent firmware
     * signature verification here (no secure-boot image signing configured),
     * so TLS (via esp_crt_bundle_attach in cap_ota_task) is the only integrity/
     * authenticity check we get. A plain http:// URL would let anyone who can
     * intercept or spoof that path push arbitrary unsigned firmware.
     */
    const char *url = cap_ota_json_str(input, "url");
    if (!url || !url[0] || strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'url' must be an https:// firmware URL (plain http:// is rejected)");
        return ESP_ERR_INVALID_ARG;
    }

    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: no second OTA partition — firmware must be built with a dual-OTA table");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *url_copy = strdup(url);
    cJSON_Delete(input);
    if (!url_copy) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    s_ota_in_progress = true;
    if (xTaskCreate(cap_ota_task, "ota_update", CAP_OTA_TASK_STACK, url_copy, 5, NULL) != pdPASS) {
        s_ota_in_progress = false;
        free(url_copy);
        snprintf(output, output_size, "Error: failed to start OTA task");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "started", true);
    cJSON_AddStringToObject(root, "note",
                            "Downloading firmware in the background; the device will reboot into the new "
                            "image on success and will not reply after reboot. Run ota_status afterwards.");
    return cap_ota_emit(root, output, output_size);
}

static esp_err_t cap_ota_status_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();

    if (app) {
        cJSON_AddStringToObject(root, "version", app->version);
        cJSON_AddStringToObject(root, "project", app->project_name);
        cJSON_AddStringToObject(root, "idf_ver", app->idf_ver);
        cJSON_AddStringToObject(root, "build_date", app->date);
        cJSON_AddStringToObject(root, "build_time", app->time);
    }
    if (running) {
        cJSON_AddStringToObject(root, "running_partition", running->label);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            cJSON_AddStringToObject(root, "running_state", cap_ota_state_str(st));
        }
    }
    cJSON_AddBoolToObject(root, "ota_capable", next != NULL);
    cJSON_AddStringToObject(root, "update_partition", next ? next->label : "");
    cJSON_AddBoolToObject(root, "update_in_progress", s_ota_in_progress);
    return cap_ota_emit(root, output, output_size);
}

static const claw_cap_descriptor_t s_ota_descriptors[] = {
    {
        .id = "ota_status",
        .name = "ota_status",
        .family = "system",
        .description = "Report firmware version/build, the running OTA partition and whether an OTA "
                       "update is possible (dual-OTA layout present).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_ota_status_execute,
    },
    {
        .id = "ota_update",
        .name = "ota_update",
        .family = "system",
        .description = "Download a firmware image from an https:// URL (e.g. on your tailnet) into the "
                       "inactive OTA slot and reboot into it. Plain http:// is rejected — TLS is the only "
                       "integrity/authenticity check since images are not otherwise signed. Restricted, "
                       "root-agent only — replaces the running firmware.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"https:// URL of the firmware .bin (http:// rejected)\"}},"
        "\"required\":[\"url\"]}",
        .execute = cap_ota_update_execute,
    },
};

static const claw_cap_group_t s_ota_group = {
    .group_id = "cap_ota",
    .descriptors = s_ota_descriptors,
    .descriptor_count = sizeof(s_ota_descriptors) / sizeof(s_ota_descriptors[0]),
};

esp_err_t cap_ota_register_group(void)
{
    if (claw_cap_group_exists(s_ota_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_ota_group);
}
