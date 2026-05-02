#include "wifi_service.h"
#include "app_events.h"
#include "app_config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_service";
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_inited = false;
static bool s_wifi_started = false;
static bool s_connected = false;
static bool s_reconnect_enabled = true;
static TickType_t s_last_reconnect_tick;

#ifndef APP_WIFI_RECONNECT_MIN_INTERVAL_MS
#define APP_WIFI_RECONNECT_MIN_INTERVAL_MS 1000
#endif

#ifndef APP_WIFI_FORCE_PUBLIC_DNS
#define APP_WIFI_FORCE_PUBLIC_DNS 1
#endif

static void wifi_service_log_one_dns(esp_netif_t *netif,
                                     esp_netif_dns_type_t dns_type,
                                     const char *label)
{
    if (!netif || !label) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    esp_err_t err = esp_netif_get_dns_info(netif, dns_type, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get dns %s failed: %s", label, esp_err_to_name(err));
        return;
    }

    if (dns.ip.type == IPADDR_TYPE_V4) {
        ESP_LOGI(TAG, "DNS %s: " IPSTR, label, IP2STR(&dns.ip.u_addr.ip4));
    } else {
        ESP_LOGI(TAG, "DNS %s: non-IPv4 type=%d", label, (int)dns.ip.type);
    }
}

static void wifi_service_log_dns_servers(esp_netif_t *netif)
{
    if (!netif) {
        ESP_LOGW(TAG, "wifi_service_log_dns_servers: netif is NULL");
        return;
    }

    wifi_service_log_one_dns(netif, ESP_NETIF_DNS_MAIN, "main");
    wifi_service_log_one_dns(netif, ESP_NETIF_DNS_BACKUP, "backup");
    wifi_service_log_one_dns(netif, ESP_NETIF_DNS_FALLBACK, "fallback");
}

static esp_err_t wifi_service_force_public_dns(esp_netif_t *netif)
{
    if (!netif) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_dns_info_t main_dns = {0};
    esp_netif_dns_info_t backup_dns = {0};

    IP4_ADDR(&main_dns.ip.u_addr.ip4, 8, 8, 8, 8);
    main_dns.ip.type = IPADDR_TYPE_V4;

    IP4_ADDR(&backup_dns.ip.u_addr.ip4, 1, 1, 1, 1);
    backup_dns.ip.type = IPADDR_TYPE_V4;

    esp_err_t err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &main_dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set main DNS failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set backup DNS failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "forced public DNS: main=8.8.8.8 backup=1.1.1.1");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_err_t err = esp_wifi_connect();
            ESP_LOGI(TAG, "wifi sta start -> connect err=%s", esp_err_to_name(err));
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)event_data;

            s_connected = false;

            app_net_status_t st = {
                .type = APP_NET_WIFI,
                .has_ip = false
            };
            esp_event_post(APP_EVENTS, APP_EVENT_NET_DOWN, &st, sizeof(st), portMAX_DELAY);

            ESP_LOGW(TAG, "wifi disconnected, reason=%d, reconnect=%d",
                     e ? e->reason : -1,
                     (int)s_reconnect_enabled);

            if (s_reconnect_enabled && s_wifi_started) {
                TickType_t now = xTaskGetTickCount();
                if ((now - s_last_reconnect_tick) >= pdMS_TO_TICKS(APP_WIFI_RECONNECT_MIN_INTERVAL_MS)) {
                    s_last_reconnect_tick = now;
                    esp_err_t err = esp_wifi_connect();
                    ESP_LOGI(TAG, "wifi reconnect request err=%s", esp_err_to_name(err));
                } else {
                    ESP_LOGW(TAG, "skip immediate reconnect because reconnect guard active");
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;

        app_net_status_t st = {
            .type = APP_NET_WIFI,
            .has_ip = true
        };

        ESP_LOGI(TAG, "wifi got ip");

#if APP_WIFI_FORCE_PUBLIC_DNS
        wifi_service_force_public_dns(s_sta_netif);
#endif
        wifi_service_log_dns_servers(s_sta_netif);

        esp_event_post(APP_EVENTS, APP_EVENT_NET_UP, &st, sizeof(st), portMAX_DELAY);
    }
}

esp_err_t wifi_service_init(void)
{
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
        s_wifi_inited = true;

        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    return ESP_OK;
}

esp_err_t wifi_service_start_sta(const char *ssid, const char *pass)
{
    wifi_config_t wifi_cfg = {0};
    esp_err_t err;

    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", pass ? pass : "");

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return err;
        }

        s_wifi_started = true;
        s_last_reconnect_tick = xTaskGetTickCount();

        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect after start failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "wifi start_sta first connect ok");
        return ESP_OK;
    }

    err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi not started, fallback to full start");

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "esp_wifi_start fallback failed: %s", esp_err_to_name(err));
            return err;
        }

        s_wifi_started = true;
        s_last_reconnect_tick = xTaskGetTickCount();

        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect fallback failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "wifi start_sta fallback connect ok");
        return ESP_OK;
    }

    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
        return err;
    }

    s_last_reconnect_tick = xTaskGetTickCount();

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start_sta reconnect failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "wifi start_sta reconnect ok");
    return ESP_OK;
}

esp_err_t wifi_service_stop_sta(void)
{
    if (!s_wifi_started) return ESP_OK;

    s_connected = false;
    s_wifi_started = false;
    return esp_wifi_stop();
}

void wifi_service_set_reconnect_enabled(bool enabled)
{
    s_reconnect_enabled = enabled;
    ESP_LOGI(TAG, "wifi reconnect enabled=%d", (int)s_reconnect_enabled);
}

bool wifi_service_is_connected(void)
{
    return s_connected;
}