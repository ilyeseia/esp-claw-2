/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_netcfg.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "cap_netcfg";

static cap_netcfg_persist_fn s_persist;
static void *s_persist_ctx;

esp_err_t cap_netcfg_set_persist_provider(cap_netcfg_persist_fn persist, void *user_ctx)
{
    s_persist = persist;
    s_persist_ctx = user_ctx;
    return ESP_OK;
}

static esp_err_t cap_netcfg_emit(cJSON *root, char *output, size_t output_size)
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

static const char *cap_netcfg_json_str(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

static bool cap_netcfg_json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
    }
    return fallback;
}

/* Returns true if s is empty or a valid dotted-decimal IPv4 address. */
static bool cap_netcfg_ip_valid(const char *s)
{
    esp_ip4_addr_t addr;
    return !s || !s[0] || esp_netif_str_to_ip4(s, &addr) == ESP_OK;
}

static esp_err_t cap_netcfg_status_execute(const char *input_json,
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

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        cJSON_AddBoolToObject(root, "available", false);
        cJSON_AddStringToObject(root, "note", "STA interface not available");
        return cap_netcfg_emit(root, output, output_size);
    }

    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(sta, &ip);
    char ipb[16] = {0}, gwb[16] = {0}, maskb[16] = {0};
    esp_ip4addr_ntoa(&ip.ip, ipb, sizeof(ipb));
    esp_ip4addr_ntoa(&ip.gw, gwb, sizeof(gwb));
    esp_ip4addr_ntoa(&ip.netmask, maskb, sizeof(maskb));

    esp_netif_dns_info_t dns_main = {0};
    esp_netif_dns_info_t dns_backup = {0};
    esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_main);
    esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_backup);
    char dnsb[16] = {0}, dns2b[16] = {0};
    esp_ip4addr_ntoa(&dns_main.ip.u_addr.ip4, dnsb, sizeof(dnsb));
    esp_ip4addr_ntoa(&dns_backup.ip.u_addr.ip4, dns2b, sizeof(dns2b));

    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    bool dhcp = (esp_netif_dhcpc_get_status(sta, &st) == ESP_OK && st == ESP_NETIF_DHCP_STARTED);

    cJSON_AddBoolToObject(root, "available", true);
    cJSON_AddStringToObject(root, "mode", dhcp ? "dhcp" : "static");
    cJSON_AddStringToObject(root, "ip", ipb);
    cJSON_AddStringToObject(root, "gateway", gwb);
    cJSON_AddStringToObject(root, "netmask", maskb);
    cJSON_AddStringToObject(root, "dns", dnsb);
    cJSON_AddStringToObject(root, "dns2", dns2b);
    return cap_netcfg_emit(root, output, output_size);
}

static esp_err_t cap_netcfg_configure_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    (void)ctx;

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *ip = cap_netcfg_json_str(input, "ip");
    const char *gateway = cap_netcfg_json_str(input, "gateway");
    const char *netmask = cap_netcfg_json_str(input, "netmask");
    const char *dns = cap_netcfg_json_str(input, "dns");
    const char *dns2 = cap_netcfg_json_str(input, "dns2");
    bool use_static = cap_netcfg_json_bool(input, "use_static", ip && ip[0]);

    /* Copy before deleting the JSON tree. */
    char ip_buf[20], gw_buf[20], mask_buf[20], dns_buf[20], dns2_buf[20];
    strlcpy(ip_buf, ip ? ip : "", sizeof(ip_buf));
    strlcpy(gw_buf, gateway ? gateway : "", sizeof(gw_buf));
    strlcpy(mask_buf, (netmask && netmask[0]) ? netmask : "255.255.255.0", sizeof(mask_buf));
    strlcpy(dns_buf, dns ? dns : "", sizeof(dns_buf));
    strlcpy(dns2_buf, dns2 ? dns2 : "", sizeof(dns2_buf));
    cJSON_Delete(input);

    if (use_static && !ip_buf[0]) {
        snprintf(output, output_size, "Error: 'ip' is required when use_static is true");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_netcfg_ip_valid(ip_buf) || !cap_netcfg_ip_valid(gw_buf) ||
            !cap_netcfg_ip_valid(mask_buf) || !cap_netcfg_ip_valid(dns_buf) ||
            !cap_netcfg_ip_valid(dns2_buf)) {
        snprintf(output, output_size, "Error: one or more fields is not a valid IPv4 address");
        return ESP_ERR_INVALID_ARG;
    }

    cap_netcfg_config_t cfg = {
        .use_static = use_static,
        .ip = ip_buf,
        .gateway = gw_buf,
        .netmask = mask_buf,
        .dns = dns_buf,
        .dns2 = dns2_buf,
    };

    bool persisted = false;
    if (s_persist) {
        esp_err_t perr = s_persist(&cfg, s_persist_ctx);
        persisted = (perr == ESP_OK);
        if (!persisted) {
            ESP_LOGW(TAG, "network config persist failed: %s", esp_err_to_name(perr));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "use_static", use_static);
    cJSON_AddStringToObject(root, "ip", ip_buf);
    cJSON_AddStringToObject(root, "gateway", gw_buf);
    cJSON_AddStringToObject(root, "netmask", mask_buf);
    cJSON_AddStringToObject(root, "dns", dns_buf);
    cJSON_AddStringToObject(root, "dns2", dns2_buf);
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "note",
                            "Saved. Restart the device to apply — changing the IP drops the current "
                            "connection, so it is not applied live.");
    return cap_netcfg_emit(root, output, output_size);
}

static const claw_cap_descriptor_t s_netcfg_descriptors[] = {
    {
        .id = "network_status",
        .name = "network_status",
        .family = "network",
        .description = "Report the current STA IP, gateway, netmask, DNS and whether DHCP or a static IP is used.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_netcfg_status_execute,
    },
    {
        .id = "network_configure",
        .name = "network_configure",
        .family = "network",
        .description = "Set a static IPv4 (ip/gateway/netmask/dns/dns2) or switch back to DHCP (use_static:false) "
                       "and persist it. Applied on the next reboot. Restricted, root-agent only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"use_static\":{\"type\":\"boolean\"},"
        "\"ip\":{\"type\":\"string\"},"
        "\"gateway\":{\"type\":\"string\"},"
        "\"netmask\":{\"type\":\"string\"},"
        "\"dns\":{\"type\":\"string\"},"
        "\"dns2\":{\"type\":\"string\"}}}",
        .execute = cap_netcfg_configure_execute,
    },
};

static const claw_cap_group_t s_netcfg_group = {
    .group_id = "cap_netcfg",
    .descriptors = s_netcfg_descriptors,
    .descriptor_count = sizeof(s_netcfg_descriptors) / sizeof(s_netcfg_descriptors[0]),
};

esp_err_t cap_netcfg_register_group(void)
{
    if (claw_cap_group_exists(s_netcfg_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_netcfg_group);
}
