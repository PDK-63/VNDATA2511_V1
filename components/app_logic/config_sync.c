#include "config_sync.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"

#include "app_events.h"
#include "app_logic.h"
#include "mqtt_service.h"
#include "runtime_config.h"

static const char *TAG = "config_sync";
static bool s_wifi_reconnect_pending = false;

#ifndef APP_HW_VERSION
#define APP_HW_VERSION "1.0.0"
#endif

#ifndef APP_FW_VERSION_STR
#define APP_FW_VERSION_STR "0.9.0.5"
#endif

static bool json_get_string(cJSON *root, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    snprintf(out, out_len, "%s", item->valuestring);
    return true;
}

static bool json_get_float(cJSON *root, const char *key, float *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = (float)item->valuedouble;
    return true;
}

static bool json_get_int(cJSON *root, const char *key, int *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = item->valueint;
    return true;
}

static bool json_get_bool(cJSON *root, const char *key, bool *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *out = cJSON_IsTrue(item);
    return true;
}

static bool float_changed(float a, float b, float eps)
{
    return fabsf(a - b) > eps;
}

static bool wifi_config_changed(const runtime_config_t *old_cfg, const runtime_config_t *new_cfg)
{
    if (old_cfg == NULL || new_cfg == NULL) {
        return false;
    }

    return (old_cfg->wifi_enabled != new_cfg->wifi_enabled) ||
           (strcmp(old_cfg->wifi_ssid, new_cfg->wifi_ssid) != 0) ||
           (strcmp(old_cfg->wifi_pass, new_cfg->wifi_pass) != 0);
}

static void publish_error_result(const char *request_id, const char *cmd_name, const char *msg)
{
    char body[320];

    int n = snprintf(body, sizeof(body),
                     "\"cmd\":\"%s\",\"ok\":false,\"msg\":\"%s\"",
                     cmd_name ? cmd_name : "",
                     msg ? msg : "");

    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "publish_error_result overflow");
        return;
    }

    mqtt_service_publish_reply(request_id ? request_id : "", body);
}

static void publish_ok_result(const char *request_id, const char *cmd_name, const char *msg)
{
    char body[320];

    int n = snprintf(body, sizeof(body),
                     "\"cmd\":\"%s\",\"ok\":true,\"msg\":\"%s\"",
                     cmd_name ? cmd_name : "",
                     msg ? msg : "");

    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "publish_ok_result overflow");
        return;
    }

    mqtt_service_publish_reply(request_id ? request_id : "", body);
}

static void publish_get_config_response(const char *request_id, const runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    char *body = (char *)calloc(1, 1800);
    if (body == NULL) {
        ESP_LOGE(TAG, "alloc response buffer failed");
        publish_error_result(request_id, "get config", "no_mem");
        return;
    }

    int n = snprintf(body, 1800,
                     "\"version\":\"%s\","
                     "\"hw_version\":\"%s\","
                     "\"wifiuser\":\"%s\","
                     "\"wifipass\":\"%s\","
                     "\"ip\":\"%s\","
                     "\"user\":\"%s\","
                     "\"pass\":\"%s\","
                     "\"sms1\":\"%s\","
                     "\"sms2\":\"%s\","
                     "\"sms3\":\"%s\","
                     "\"sms4\":\"%s\","
                     "\"sms5\":\"%s\","
                     "\"message\":\"%s\","
                     "\"phone1\":\"%s\","
                     "\"phone2\":\"\","
                     "\"phone3\":\"\","
                     "\"phone4\":\"\","
                     "\"phone5\":\"\","
                     "\"ahi\":%.2f,"
                     "\"alo\":%.2f,"
                     "\"humhi\":%.2f,"
                     "\"humlo\":%.2f,"
                     "\"calib\":%.2f,"
                     "\"calib_ntc_pcb\":%.2f,"
                     "\"calib_temp_sht\":%.2f,"
                     "\"calib_hum_sht\":%.2f,"
                     "\"time_delay\":%d,"
                     "\"gps\":%s,"
                     "\"apn\":\"%s\"",
                     APP_FW_VERSION_STR,
                     APP_HW_VERSION,
                     cfg->wifi_ssid,
                     cfg->wifi_pass,
                     cfg->broker_uri,
                     cfg->mqtt_username,
                     cfg->mqtt_password,
                     cfg->alarm_number2,
                     cfg->alarm_number3,
                     cfg->alarm_number4,
                     cfg->alarm_number5,
                     cfg->alarm_number6,
                     cfg->message,
                     cfg->alarm_number1,
                     cfg->ntc_high_limit_c,
                     cfg->ntc_low_limit_c,
                     cfg->hum_high_limit_pct,
                     cfg->hum_low_limit_pct,
                     cfg->ntc_calib_c,
                     cfg->ntc_calib_c,
                     0.0f,
                     cfg->hum_calib_pct,
                     cfg->telemetry_interval_ms,
                     cfg->gps_enabled ? "true" : "false",
                     cfg->apn);

    if (n < 0 || n >= 1800) {
        ESP_LOGE(TAG, "get config response too long");
        free(body);
        publish_error_result(request_id, "get config", "response_too_long");
        return;
    }

    mqtt_service_publish_reply(request_id ? request_id : "", body);
    free(body);
}

static void handle_set_config(const app_cloud_cmd_t *cmd)
{
    runtime_config_t cfg = {0};
    runtime_config_t old_cfg = {0};

    if (cmd == NULL || !cmd->params[0]) {
        publish_error_result(cmd ? cmd->request_id : "", "set config", "empty_params");
        return;
    }

    cJSON *root = cJSON_Parse(cmd->params);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid json for set config");
        publish_error_result(cmd->request_id, "set config", "invalid_json");
        return;
    }

    if (runtime_config_load(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
    }
    old_cfg = cfg;

    json_get_string(root, "ip", cfg.broker_uri, sizeof(cfg.broker_uri));
    json_get_string(root, "user", cfg.mqtt_username, sizeof(cfg.mqtt_username));
    json_get_string(root, "pass", cfg.mqtt_password, sizeof(cfg.mqtt_password));

    if (json_get_string(root, "wifiuser", cfg.wifi_ssid, sizeof(cfg.wifi_ssid))) {
        cfg.wifi_enabled = (cfg.wifi_ssid[0] != '\0');
    }
    json_get_string(root, "wifipass", cfg.wifi_pass, sizeof(cfg.wifi_pass));

    json_get_string(root, "phone1", cfg.alarm_number1, sizeof(cfg.alarm_number1));
    json_get_string(root, "sms1", cfg.alarm_number2, sizeof(cfg.alarm_number2));
    json_get_string(root, "sms2", cfg.alarm_number3, sizeof(cfg.alarm_number3));
    json_get_string(root, "sms3", cfg.alarm_number4, sizeof(cfg.alarm_number4));
    json_get_string(root, "sms4", cfg.alarm_number5, sizeof(cfg.alarm_number5));
    json_get_string(root, "sms5", cfg.alarm_number6, sizeof(cfg.alarm_number6));

    json_get_string(root, "message", cfg.message, sizeof(cfg.message));

    json_get_float(root, "ahi", &cfg.ntc_high_limit_c);
    json_get_float(root, "alo", &cfg.ntc_low_limit_c);
    json_get_float(root, "humhi", &cfg.hum_high_limit_pct);
    json_get_float(root, "humlo", &cfg.hum_low_limit_pct);

    json_get_string(root, "apn", cfg.apn, sizeof(cfg.apn));
    json_get_bool(root, "gps", &cfg.gps_enabled);
    
    cJSON_Delete(root);

    bool wifi_changed = wifi_config_changed(&old_cfg, &cfg);

    bool temp_limit_changed =
        float_changed(old_cfg.ntc_low_limit_c, cfg.ntc_low_limit_c, 0.01f) ||
        float_changed(old_cfg.ntc_high_limit_c, cfg.ntc_high_limit_c, 0.01f);

    bool hum_limit_changed =
        float_changed(old_cfg.hum_low_limit_pct, cfg.hum_low_limit_pct, 0.01f) ||
        float_changed(old_cfg.hum_high_limit_pct, cfg.hum_high_limit_pct, 0.01f);

    if (runtime_config_save(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_save failed");
        publish_error_result(cmd->request_id, "set config", "save_failed");
        return;
    }

    app_logic_load_runtime_config();

    if (temp_limit_changed || hum_limit_changed) {
        app_logic_handle_web_limit_update(old_cfg.ntc_low_limit_c,
                                          old_cfg.ntc_high_limit_c,
                                          old_cfg.hum_low_limit_pct,
                                          old_cfg.hum_high_limit_pct);
    }

    s_wifi_reconnect_pending = wifi_changed;

    ESP_LOGW(TAG,
             "set config ok: wifi_changed=%d temp_limit_changed=%d hum_limit_changed=%d ahi=%.2f alo=%.2f humhi=%.2f humlo=%.2f",
             wifi_changed ? 1 : 0,
             temp_limit_changed ? 1 : 0,
             hum_limit_changed ? 1 : 0,
             cfg.ntc_high_limit_c,
             cfg.ntc_low_limit_c,
             cfg.hum_high_limit_pct,
             cfg.hum_low_limit_pct);

    publish_ok_result(cmd->request_id, "set config", "saved");
}

static void handle_admin_set_config(const app_cloud_cmd_t *cmd)
{
    runtime_config_t cfg = {0};

    if (cmd == NULL || !cmd->params[0]) {
        publish_error_result(cmd ? cmd->request_id : "", "admin set config", "empty_params");
        return;
    }

    cJSON *root = cJSON_Parse(cmd->params);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid json for admin set config");
        publish_error_result(cmd->request_id, "admin set config", "invalid_json");
        return;
    }

    if (runtime_config_load(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
    }

    json_get_float(root, "calib", &cfg.ntc_calib_c);
    json_get_int(root, "time_delay", &cfg.telemetry_interval_ms);

    cJSON_Delete(root);

    if (runtime_config_save(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_save failed");
        publish_error_result(cmd->request_id, "admin set config", "save_failed");
        return;
    }

    app_logic_load_runtime_config();

    ESP_LOGW(TAG,
             "admin set config ok: calib=%.2f time_delay=%d",
             cfg.ntc_calib_c,
             cfg.telemetry_interval_ms);

    publish_get_config_response(cmd->request_id, &cfg);

    if (s_wifi_reconnect_pending) {
        esp_err_t err = esp_event_post(APP_EVENTS,
                                       APP_EVENT_WIFI_CONFIG_SAVED,
                                       NULL,
                                       0,
                                       portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "APP_EVENT_WIFI_CONFIG_SAVED post failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "posted APP_EVENT_WIFI_CONFIG_SAVED");
            s_wifi_reconnect_pending = false;
        }
    }
}

static void handle_get_config(const app_cloud_cmd_t *cmd)
{
    runtime_config_t cfg = {0};

    if (runtime_config_load(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_load failed");
        publish_error_result(cmd ? cmd->request_id : "", "get config", "load_failed");
        return;
    }

    publish_get_config_response(cmd ? cmd->request_id : "", &cfg);
}

void config_sync_handle_cloud_command(const app_cloud_cmd_t *cmd)
{
    if (cmd == NULL || cmd->cmd[0] == '\0') {
        return;
    }

    ESP_LOGW(TAG, "cloud cmd=%s params=%s request_id=%s",
             cmd->cmd,
             cmd->params,
             cmd->request_id);

    if (strcmp(cmd->cmd, "set config") == 0) {
        handle_set_config(cmd);
        return;
    }

    if (strcmp(cmd->cmd, "admin set config") == 0) {
        handle_admin_set_config(cmd);
        return;
    }

    if (strcmp(cmd->cmd, "get config") == 0) {
        handle_get_config(cmd);
        return;
    }

    publish_error_result(cmd->request_id, cmd->cmd, "unsupported_cmd");
}

void config_sync_publish_current_config_for_web(const char *request_id)
{
    runtime_config_t cfg = {0};

    if (runtime_config_load(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_load failed");
        return;
    }

    publish_get_config_response(request_id ? request_id : "sms", &cfg);
}