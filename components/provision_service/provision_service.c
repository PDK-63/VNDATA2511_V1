#include "provision_service.h"
#include "runtime_config.h"
#include "app_events.h"
#include "app_config.h"
#include "mqtt_service.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_check.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "provision";
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_httpd = NULL;
static bool s_running = false;

#ifndef APP_PROVISION_SCAN_MAX_AP
#define APP_PROVISION_SCAN_MAX_AP 20
#endif

static void apply_wifi_config_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = esp_event_post(APP_EVENTS, APP_EVENT_WIFI_CONFIG_SAVED, NULL, 0, portMAX_DELAY);
    ESP_LOGI(TAG, "deferred wifi config apply posted err=%s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html_prefix[] =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>VN2511 WiFi Config</title>"
        "<style>"
        "body{font-family:Arial;background:#f3f3f3;margin:0;padding:20px;}"
        ".card{max-width:380px;margin:20px auto;background:#fff;padding:24px;border-radius:10px;box-shadow:0 2px 8px rgba(0,0,0,.12);}"
        "h1{font-size:28px;margin-bottom:20px;}"
        "label{display:block;margin:16px 0 8px;font-weight:600;color:#444;}"
        "input[type=text],input[type=password],select{width:100%;padding:12px;border:1px solid #ccc;border-radius:4px;font-size:16px;box-sizing:border-box;}"
        ".row{margin-top:12px;color:#444;}"
        ".btn-secondary{margin-top:10px;background:#1976d2;color:#fff;border:none;padding:10px 16px;border-radius:4px;font-size:15px;}"
        "button[type=submit]{margin-top:22px;background:#4CAF50;color:#fff;border:none;padding:12px 24px;border-radius:4px;font-size:18px;}"
        ".hint{font-size:13px;color:#666;margin-top:6px;}"
        "</style>"
        "<script>"
        "function togglePwd(){var p=document.getElementById('pwd'); p.type=(p.type==='password')?'text':'password';}"
        "function pickSsid(){var sel=document.getElementById('ssid_list'); if(sel && sel.value){document.getElementById('ssid').value=sel.value;}}"
        "async function scanWifi(){"
        " var sel=document.getElementById('ssid_list');"
        " var hint=document.getElementById('scan_hint');"
        " sel.innerHTML='<option value=\"\">Dang quet...</option>';"
        " hint.textContent='Dang do WiFi...';"
        " try{"
        "   const r=await fetch('/scan');"
        "   const j=await r.json();"
        "   sel.innerHTML='<option value=\"\">-- Chon WiFi --</option>';"
        "   if(j.ok && j.aps){"
        "     j.aps.forEach(function(ap){"
        "       var o=document.createElement('option');"
        "       var name=(ap.ssid && ap.ssid.length)?ap.ssid:'(hidden)';"
        "       o.value=ap.ssid || '';"
        "       o.text=name+' ('+ap.rssi+' dBm)';"
        "       sel.appendChild(o);"
        "     });"
        "     hint.textContent='Tim thay '+j.aps.length+' mang.';"
        "   }else{"
        "     hint.textContent='Khong quet duoc mang WiFi.';"
        "   }"
        " }catch(e){"
        "   sel.innerHTML='<option value=\"\">-- Loi quet WiFi --</option>';"
        "   hint.textContent='Quet that bai.';"
        " }"
        "}"
        "window.addEventListener('load', function(){ scanWifi(); });"
        "</script></head><body>"
        "<div class='card'>"
        "<h1>VN2511 WiFi Config</h1>"
        "<label>Danh sach WiFi</label>"
        "<select id='ssid_list' onchange='pickSsid()'><option value=''>-- Chon WiFi --</option></select>"
        "<button class='btn-secondary' type='button' onclick='scanWifi()'>Scan WiFi</button>"
        "<div id='scan_hint' class='hint'>San sang quet.</div>"
        "<form method='POST' action='/save'>"
        "<label>WiFi SSID</label><input id='ssid' name='ssid' type='text' required>"
        "<label>WiFi Password</label><input id='pwd' name='password' type='password'>"
        "<div class='row'><input type='checkbox' onclick='togglePwd()'> Show password</div>"
        "<label>Device ID</label><input type='text' value='";
    static const char html_suffix[] =
        "' readonly>"
        "<button type='submit'>Save</button>"
        "</form></div></body></html>";

    const char *device_id = mqtt_service_get_device_key();
    if (!device_id || device_id[0] == '\0') {
        device_id = APP_DEVICE_ID;
    }

    httpd_resp_set_type(req, "text/html");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, html_prefix), TAG, "send html prefix failed");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, device_id), TAG, "send device id failed");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, html_suffix), TAG, "send html suffix failed");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i + 1 < maxlen) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    char ssid[64] = {0};
    char pass[64] = {0};
    runtime_config_t cfg;

    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    }

    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
    }
    buf[r] = '\0';

    char *ssid_p = strstr(buf, "ssid=");
    char *pass_p = strstr(buf, "password=");

    if (ssid_p) {
        ssid_p += 5;
        char *end = strchr(ssid_p, '&');
        if (end) *end = '\0';
        url_decode(ssid, ssid_p, sizeof(ssid));
        if (end) *end = '&';
    }

    if (pass_p) {
        pass_p += 9;
        char *end = strchr(pass_p, '&');
        if (end) *end = '\0';
        url_decode(pass, pass_p, sizeof(pass));
        if (end) *end = '&';
    }

    if (ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }

    if (runtime_config_load(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
    }

    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", pass);
    cfg.wifi_enabled = true;

    esp_err_t save_err = runtime_config_save(&cfg);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_save failed: %s", esp_err_to_name(save_err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save fail");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h2>Saved. Device is applying the new Wi-Fi configuration...</h2></body></html>");

    BaseType_t ok = xTaskCreate(apply_wifi_config_task,
                                "prov_apply_wifi",
                                3072,
                                NULL,
                                tskIDLE_PRIORITY + 1,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create apply task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_start_failed\"}");
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_count_failed\"}");
        return ESP_OK;
    }

    if (ap_count > APP_PROVISION_SCAN_MAX_AP) {
        ap_count = APP_PROVISION_SCAN_MAX_AP;
    }

    wifi_ap_record_t *recs = calloc(ap_count ? ap_count : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_mem\"}");
        return ESP_OK;
    }

    uint16_t num = ap_count;
    err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err != ESP_OK) {
        free(recs);
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_read_failed\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"aps\":[");
    for (int i = 0; i < num; i++) {
        char item[224];
        snprintf(item, sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"channel\":%d}",
                 (i == 0) ? "" : ",",
                 (const char *)recs[i].ssid,
                 recs[i].rssi,
                 recs[i].authmode,
                 recs[i].primary);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);

    free(recs);
    ESP_LOGI(TAG, "scan complete, ap_count=%u", (unsigned)num);
    return ESP_OK;
}

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_get_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_httpd, &root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register root handler failed: %s", esp_err_to_name(err));
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_httpd, &save);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register save handler failed: %s", esp_err_to_name(err));
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_httpd, &scan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register scan handler failed: %s", esp_err_to_name(err));
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return err;
    }

    return ESP_OK;
}

static void stop_http_if_running(void)
{
    if (s_httpd) {
        ESP_LOGI(TAG, "stopping provision http server");
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

static esp_err_t restart_softap_clean(void)
{
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(err));
    }

    stop_http_if_running();

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_STOP_STATE) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(300));

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "failed to create AP netif");
            return ESP_FAIL;
        }
    }

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP ip info failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", APP_WIFI_AP_SSID);
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", APP_WIFI_AP_PASS);
    ap_cfg.ap.ssid_len = strlen(APP_WIFI_AP_SSID);
    ap_cfg.ap.channel = APP_WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = APP_WIFI_AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    if (strlen(APP_WIFI_AP_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    /* APSTA de vua phat AP local, vua scan duoc mang xung quanh */
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = start_http();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provision http start failed after AP restart: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG,
             "SoftAP restarted cleanly at http://192.168.4.1 ssid=%s channel=%d prev_mode=%d",
             APP_WIFI_AP_SSID,
             APP_WIFI_AP_CHANNEL,
             (int)current_mode);

    return ESP_OK;
}

esp_err_t provision_service_start(void)
{
    esp_err_t err = restart_softap_clean();
    if (err != ESP_OK) {
        s_running = false;
        return err;
    }

    app_net_status_t st = {.type = APP_NET_AP_ONLY, .has_ip = true};
    err = esp_event_post(APP_EVENTS, APP_EVENT_NET_UP, &st, sizeof(st), portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APP_EVENT_NET_UP post failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t provision_service_stop(void)
{
    stop_http_if_running();
    s_running = false;

    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_STOP_STATE) {
        return ESP_OK;
    }
    return err;
}

bool provision_service_is_running(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        return s_running;
    }
    return false;
}
