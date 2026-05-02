
#include "runtime_config.h"
#include "app_config.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#define NS "runtime_cfg"

static void set_defaults(runtime_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->apn, sizeof(cfg->apn), "%s", APP_MODEM_APN);
    snprintf(cfg->broker_uri, sizeof(cfg->broker_uri), "%s", APP_MQTT_URI);
    snprintf(cfg->mqtt_username, sizeof(cfg->mqtt_username), "%s", APP_MQTT_USERNAME);
    snprintf(cfg->mqtt_password, sizeof(cfg->mqtt_password), "%s", APP_MQTT_PASSWORD);

    snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", "");
    snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", "");
    cfg->wifi_enabled = false;

    cfg->telemetry_interval_ms = APP_DEFAULT_PUB_MS;

    snprintf(cfg->alarm_number1, sizeof(cfg->alarm_number1), "%s", "0123456789");
    snprintf(cfg->alarm_number2, sizeof(cfg->alarm_number2), "%s", "0123456789");
    snprintf(cfg->alarm_number3, sizeof(cfg->alarm_number3), "%s", "0123456789");
    snprintf(cfg->alarm_number4, sizeof(cfg->alarm_number4), "%s", "0123456789");
    snprintf(cfg->alarm_number5, sizeof(cfg->alarm_number5), "%s", "0123456789");
    snprintf(cfg->alarm_number6, sizeof(cfg->alarm_number6), "%s", "0123456789");

    cfg->ntc_low_limit_c = APP_NTC_LOW_LIMIT_C;
    cfg->ntc_high_limit_c = APP_NTC_HIGH_LIMIT_C;
    cfg->ntc_calib_c = 0.0f;

    cfg->hum_low_limit_pct = APP_HUM_LOW_LIMIT_PCT;
    cfg->hum_high_limit_pct = APP_HUM_HIGH_LIMIT_PCT;
    cfg->hum_calib_pct = 0.0f;
    cfg->hum_enabled = true;
    snprintf(cfg->message, sizeof(cfg->message), "%s", "");
    cfg->gps_enabled = true;
}

static esp_err_t save_str(nvs_handle_t h, const char *key, const char *val)
{
    return nvs_set_str(h, key, val ? val : "");
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size, const char *def)
{
    size_t req = 0;
    memset(dst, 0, size);

    if (nvs_get_str(h, key, NULL, &req) == ESP_OK && req > 0 && req <= size) {
        nvs_get_str(h, key, dst, &req);
        dst[size - 1] = '\0';
    } else {
        snprintf(dst, size, "%s", def ? def : "");
    }
}

static void sanitize_config(runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (cfg->telemetry_interval_ms < APP_MIN_PUB_MS ||
        cfg->telemetry_interval_ms > APP_MAX_PUB_MS) {
        cfg->telemetry_interval_ms = APP_DEFAULT_PUB_MS;
    }

    if (cfg->ntc_low_limit_c < -55.0f || cfg->ntc_low_limit_c > 100.0f) {
        cfg->ntc_low_limit_c = APP_NTC_LOW_LIMIT_C;
    }

    if (cfg->ntc_high_limit_c < -55.0f || cfg->ntc_high_limit_c > 100.0f) {
        cfg->ntc_high_limit_c = APP_NTC_HIGH_LIMIT_C;
    }

    if (cfg->ntc_high_limit_c <= cfg->ntc_low_limit_c) {
        cfg->ntc_low_limit_c = APP_NTC_LOW_LIMIT_C;
        cfg->ntc_high_limit_c = APP_NTC_HIGH_LIMIT_C;
    }

    if (cfg->ntc_calib_c < -20.0f || cfg->ntc_calib_c > 20.0f) {
        cfg->ntc_calib_c = 0.0f;
    }

    if (cfg->hum_low_limit_pct < 0.0f || cfg->hum_low_limit_pct > 100.0f) {
        cfg->hum_low_limit_pct = APP_HUM_LOW_LIMIT_PCT;
    }

    if (cfg->hum_high_limit_pct < 0.0f || cfg->hum_high_limit_pct > 100.0f) {
        cfg->hum_high_limit_pct = APP_HUM_HIGH_LIMIT_PCT;
    }

    if (cfg->hum_high_limit_pct <= cfg->hum_low_limit_pct) {
        cfg->hum_low_limit_pct = APP_HUM_LOW_LIMIT_PCT;
        cfg->hum_high_limit_pct = APP_HUM_HIGH_LIMIT_PCT;
    }

    if (cfg->hum_calib_pct < -20.0f || cfg->hum_calib_pct > 20.0f) {
        cfg->hum_calib_pct = 0.0f;
    }
}

esp_err_t runtime_config_init(void)
{
    runtime_config_t cfg;
    esp_err_t err = runtime_config_load(&cfg);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    set_defaults(&cfg);
    return runtime_config_save(&cfg);
}

esp_err_t runtime_config_load(runtime_config_t *cfg)
{
    nvs_handle_t h;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    set_defaults(cfg);

    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        sanitize_config(cfg);
        return ESP_OK;
    }

    load_str(h, "apn", cfg->apn, sizeof(cfg->apn), APP_MODEM_APN);
    load_str(h, "broker", cfg->broker_uri, sizeof(cfg->broker_uri), APP_MQTT_URI);
    load_str(h, "user", cfg->mqtt_username, sizeof(cfg->mqtt_username), APP_MQTT_USERNAME);
    load_str(h, "pass", cfg->mqtt_password, sizeof(cfg->mqtt_password), APP_MQTT_PASSWORD);

    load_str(h, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "");
    load_str(h, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass), "");
    load_str(h, "message", cfg->message, sizeof(cfg->message), "");

    load_str(h, "call_num", cfg->alarm_number1, sizeof(cfg->alarm_number1), "0123456789");
    load_str(h, "sms1_num", cfg->alarm_number2, sizeof(cfg->alarm_number2), "0123456789");
    load_str(h, "sms2_num", cfg->alarm_number3, sizeof(cfg->alarm_number3), "0123456789");
    load_str(h, "sms3_num", cfg->alarm_number4, sizeof(cfg->alarm_number4), "0123456789");
    load_str(h, "sms4_num", cfg->alarm_number5, sizeof(cfg->alarm_number5), "0123456789");
    load_str(h, "sms5_num", cfg->alarm_number6, sizeof(cfg->alarm_number6), "0123456789");

    uint8_t wifi_enabled = 0;
    if (nvs_get_u8(h, "wifi_en", &wifi_enabled) == ESP_OK) {
        cfg->wifi_enabled = (wifi_enabled != 0);
    }

    uint8_t gps_enabled = 1;
    if (nvs_get_u8(h, "gps_en", &gps_enabled) == ESP_OK) {
        cfg->gps_enabled = (gps_enabled != 0);
    }

    int32_t pub_ms = APP_DEFAULT_PUB_MS;
    if (nvs_get_i32(h, "pub_ms", &pub_ms) == ESP_OK) {
        cfg->telemetry_interval_ms = pub_ms;
    }

    size_t sz = 0;

    sz = sizeof(cfg->ntc_low_limit_c);
    (void)nvs_get_blob(h, "ntc_low", &cfg->ntc_low_limit_c, &sz);

    sz = sizeof(cfg->ntc_high_limit_c);
    (void)nvs_get_blob(h, "ntc_high", &cfg->ntc_high_limit_c, &sz);

    sz = sizeof(cfg->ntc_calib_c);
    (void)nvs_get_blob(h, "ntc_cal", &cfg->ntc_calib_c, &sz);

    sz = sizeof(cfg->hum_low_limit_pct);
    (void)nvs_get_blob(h, "hum_low", &cfg->hum_low_limit_pct, &sz);

    sz = sizeof(cfg->hum_high_limit_pct);
    (void)nvs_get_blob(h, "hum_high", &cfg->hum_high_limit_pct, &sz);

    sz = sizeof(cfg->hum_calib_pct);
    (void)nvs_get_blob(h, "hum_cal", &cfg->hum_calib_pct, &sz);

    uint8_t hum_enabled = 1;
    if (nvs_get_u8(h, "hum_en", &hum_enabled) == ESP_OK) {
        cfg->hum_enabled = (hum_enabled != 0);
    }
    nvs_close(h);

    sanitize_config(cfg);
    return ESP_OK;
}

esp_err_t runtime_config_save(const runtime_config_t *cfg)
{
    nvs_handle_t h;
    runtime_config_t tmp;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    tmp = *cfg;
    sanitize_config(&tmp);

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = save_str(h, "apn", tmp.apn);
    if (err != ESP_OK) goto out;

    err = save_str(h, "broker", tmp.broker_uri);
    if (err != ESP_OK) goto out;

    err = save_str(h, "user", tmp.mqtt_username);
    if (err != ESP_OK) goto out;

    err = save_str(h, "pass", tmp.mqtt_password);
    if (err != ESP_OK) goto out;

    err = save_str(h, "wifi_ssid", tmp.wifi_ssid);
    if (err != ESP_OK) goto out;

    err = save_str(h, "wifi_pass", tmp.wifi_pass);
    if (err != ESP_OK) goto out;

    err = save_str(h, "message", tmp.message);
    if (err != ESP_OK) goto out;

    err = save_str(h, "call_num", tmp.alarm_number1);
    if (err != ESP_OK) goto out;

    err = save_str(h, "sms1_num", tmp.alarm_number2);
    if (err != ESP_OK) goto out;

    err = save_str(h, "sms2_num", tmp.alarm_number3);
    if (err != ESP_OK) goto out;

    err = save_str(h, "sms3_num", tmp.alarm_number4);
    if (err != ESP_OK) goto out;

    err = save_str(h, "sms4_num", tmp.alarm_number5);
    if (err != ESP_OK) goto out;

    err = save_str(h, "sms5_num", tmp.alarm_number6);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(h, "wifi_en", tmp.wifi_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(h, "gps_en", tmp.gps_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(h, "pub_ms", tmp.telemetry_interval_ms);
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "ntc_low", &tmp.ntc_low_limit_c, sizeof(tmp.ntc_low_limit_c));
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "ntc_high", &tmp.ntc_high_limit_c, sizeof(tmp.ntc_high_limit_c));
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "ntc_cal", &tmp.ntc_calib_c, sizeof(tmp.ntc_calib_c));
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "hum_low", &tmp.hum_low_limit_pct, sizeof(tmp.hum_low_limit_pct));
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "hum_high", &tmp.hum_high_limit_pct, sizeof(tmp.hum_high_limit_pct));
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, "hum_cal", &tmp.hum_calib_pct, sizeof(tmp.hum_calib_pct));
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(h, "hum_en", tmp.hum_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;
    err = nvs_commit(h);

out:
    nvs_close(h);
    return err;
}

esp_err_t runtime_config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}