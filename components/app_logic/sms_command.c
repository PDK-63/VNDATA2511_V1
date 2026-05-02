
#include "sms_command.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_logic_ctx.h"
#include "app_config.h"
#include "mqtt_service.h"
#include "modem_service.h"

#include "config_sync.h"
#include "app_logic.h"
static const char *TAG = "sms_command";

static void trim_spaces(char *s)
{
    if (!s || !s[0]) {
        return;
    }

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        memmove(s, s + 1, strlen(s));
    }

    int len = (int)strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' ||
            s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[len - 1] = '\0';
        len--;
    }
}

static int split_sms_fields(char *input, char *fields[], int max_fields)
{
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(input, ";", &saveptr);

    while (tok && count < max_fields) {
        trim_spaces(tok);
        fields[count++] = tok;
        tok = strtok_r(NULL, ";", &saveptr);
    }
    return count;
}

static bool is_placeholder_alarm_number(const char *s)
{
    return s && strcmp(s, "0123456789") == 0;
}

static bool is_all_digits(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }

    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        s++;
    }
    return true;
}

static bool is_valid_vn_mobile_prefix(const char *s)
{
    if (!s) {
        return false;
    }

    /* So di dong Viet Nam dang 10 so:
       03x, 05x, 07x, 08x, 09x */
    if (strncmp(s, "03", 2) == 0) return true;
    if (strncmp(s, "05", 2) == 0) return true;
    if (strncmp(s, "07", 2) == 0) return true;
    if (strncmp(s, "08", 2) == 0) return true;
    if (strncmp(s, "09", 2) == 0) return true;

    return false;
}

static bool is_valid_alarm_phone_field(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }

    /* Placeholder van duoc phep luu de bieu thi khong muon nhan alarm */
    if (is_placeholder_alarm_number(s)) {
        return true;
    }

    if (!is_all_digits(s)) {
        return false;
    }

    if (strlen(s) != 10) {
        return false;
    }

    if (s[0] != '0') {
        return false;
    }

    if (!is_valid_vn_mobile_prefix(s)) {
        return false;
    }

    return true;
}


bool sms_command_text_equals_ci(const char *text, const char *expected)
{
    if (!text || !expected) {
        return false;
    }

    while (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t') {
        text++;
    }

    size_t tlen = strlen(text);
    while (tlen > 0 &&
           (text[tlen - 1] == ' ' || text[tlen - 1] == '\r' ||
            text[tlen - 1] == '\n' || text[tlen - 1] == '\t')) {
        tlen--;
    }

    size_t elen = strlen(expected);
    if (tlen != elen) {
        return false;
    }

    for (size_t i = 0; i < elen; ++i) {
        char a = text[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

void sms_command_format_time(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    bool have_real_time = (localtime_r(&now, &tm_now) != NULL) &&
                          (tm_now.tm_year >= (2026 - 1900));

    if (have_real_time) {
        strftime(out, out_len, "%Hh%M %d/%m/%Y", &tm_now);
    } else {
        snprintf(out, out_len, "Uptime %llus",
                 (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    }
}

void sms_command_format_eng_info(char *out, size_t out_len)
{
    app_logic_ctx_t ctx;
    app_logic_get_context(&ctx);

    const char *ssid = ctx.runtime_cfg->wifi_ssid[0] ? ctx.runtime_cfg->wifi_ssid : "NoWiFi";
    const char *net = app_logic_net_type_to_str(*ctx.net_type);

    snprintf(out, out_len,
             "%s; WiFi:%s; Net:%s; WiFiEn:%d;",
             mqtt_service_get_device_key(),
             ssid,
             net,
             ctx.runtime_cfg->wifi_enabled ? 1 : 0);
}

void sms_command_format_status(char *out, size_t out_len)
{
    app_logic_ctx_t ctx;
    app_logic_get_context(&ctx);

    time_t now = time(NULL);
    struct tm tm_now;
    bool have_real_time = localtime_r(&now, &tm_now) != NULL &&
                          tm_now.tm_year >= (2026 - 1900);

    char time_buf[48];
    if (have_real_time) {
        strftime(time_buf, sizeof(time_buf), "%Hh%M - %d/%m/%Y", &tm_now);
    } else {
        snprintf(time_buf, sizeof(time_buf), "Uptime %llus",
                 (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    }

    const char *power_state = app_logic_get_power_status_text();
    //float temp_to_send = *ctx.ntc1_valid ? *ctx.last_temp1_c : 0.0f;
    float temp_to_send = *ctx.last_temp1_c;
    float hum_to_send = ctx.runtime_cfg->hum_enabled
                    ? *ctx.last_humidity : 0.0f;
    snprintf(out, out_len,
             "%s (%s); %.2f (%.0f-%.0f); %.0f%% (%.0f-%.0f); %s; %s.",
             mqtt_service_get_device_key(),
             APP_FW_VERSION,
             temp_to_send,
             ctx.runtime_cfg->ntc_low_limit_c,
             ctx.runtime_cfg->ntc_high_limit_c,
             hum_to_send,
             ctx.runtime_cfg->hum_low_limit_pct,
             ctx.runtime_cfg->hum_high_limit_pct,
             power_state,
             time_buf);
}

esp_err_t sms_command_process_set(const char *sender, const char *body)
{
    char work[192];
    char *f[10];
    int n;
    int cmd_index = -1;
    app_logic_ctx_t ctx;

    if (!sender || !body) {
        return ESP_ERR_INVALID_ARG;
    }

    app_logic_get_context(&ctx);

    snprintf(work, sizeof(work), "%s", body);
    n = split_sms_fields(work, f, 10);

    if (n < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(f[0], "2511") != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (n >= 3 && strcmp(f[1], "SET") == 0) {
        cmd_index = 2;
    } else if (n >= 2) {
        cmd_index = 1;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    runtime_config_t cfg;
    if (runtime_config_load(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
    }

    if (strcmp(f[cmd_index], "10") == 0) {
        int ssid_idx = cmd_index + 1;
        int pass_idx = cmd_index + 2;

        if (n <= pass_idx) {
            modem_service_send_sms(sender, "SET 10 FAIL");
            return ESP_ERR_INVALID_ARG;
        }

        snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", f[ssid_idx]);
        snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", f[pass_idx]);
        cfg.wifi_enabled = true;

        esp_err_t err = runtime_config_save(&cfg);
        if (err != ESP_OK) {
            modem_service_send_sms(sender, "SET 10 FAIL");
            return err;
        }

        app_logic_load_runtime_config();

        config_sync_publish_current_config_for_web("web");

        for (int i = 0; i < 50; i++) {
            if (mqtt_service_is_connected() && mqtt_service_get_outbox_size() == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        modem_service_send_sms(sender, "SET 10 OK, REBOOT");
        ESP_LOGI(TAG, "wifi config updated ssid=[%s], rebooting", cfg.wifi_ssid);

        vTaskDelay(pdMS_TO_TICKS(7000));
        esp_restart();
        return ESP_OK;
    }

    if (strcmp(f[cmd_index], "11") == 0) {
        int first_num_idx = cmd_index + 1;

        if (n < first_num_idx + 6) {
            modem_service_send_sms(sender, "SET 11 FAIL: THIEU SO, CAN 6 SDT");
            return ESP_ERR_INVALID_ARG;
        }

        if (n > first_num_idx + 6) {
            modem_service_send_sms(sender, "SET 11 FAIL: THUA SO, CHI NHAN 6 SDT");
            return ESP_ERR_INVALID_ARG;
        }

        for (int i = 0; i < 6; i++) {
            if (!is_valid_alarm_phone_field(f[first_num_idx + i])) {
                char msg[64];
                snprintf(msg, sizeof(msg), "SET 11 FAIL: SDT%d PHAI DUNG 10 SO", i + 1);
                modem_service_send_sms(sender, msg);
                return ESP_ERR_INVALID_ARG;
            }
        }

        snprintf(cfg.alarm_number1, sizeof(cfg.alarm_number1), "%s", f[first_num_idx + 0]);
        snprintf(cfg.alarm_number2, sizeof(cfg.alarm_number2), "%s", f[first_num_idx + 1]);
        snprintf(cfg.alarm_number3, sizeof(cfg.alarm_number3), "%s", f[first_num_idx + 2]);
        snprintf(cfg.alarm_number4, sizeof(cfg.alarm_number4), "%s", f[first_num_idx + 3]);
        snprintf(cfg.alarm_number5, sizeof(cfg.alarm_number5), "%s", f[first_num_idx + 4]);
        snprintf(cfg.alarm_number6, sizeof(cfg.alarm_number6), "%s", f[first_num_idx + 5]);

        esp_err_t err = runtime_config_save(&cfg);
        modem_service_send_sms(sender, err == ESP_OK ? "SET 11 OK" : "SET 11 FAIL: SAVE ERROR");
        if (err == ESP_OK) {
            app_logic_load_runtime_config();
            config_sync_publish_current_config_for_web("sms");
        }
        return err;
    }

    if (strcmp(f[cmd_index], "20") == 0) {
        int low_idx = cmd_index + 1;
        int high_idx = cmd_index + 2;
        int calib_idx = cmd_index + 3;

        if (n <= calib_idx) {
            modem_service_send_sms(sender, "SET 20 FAIL");
            return ESP_ERR_INVALID_ARG;
        }

        float low = strtof(f[low_idx], NULL);
        float high = strtof(f[high_idx], NULL);
        float calib = strtof(f[calib_idx], NULL);

        if (low < -55.0f || low > 100.0f ||
            high < -55.0f || high > 100.0f ||
            low >= high ||
            calib < -20.0f || calib > 20.0f) {
            modem_service_send_sms(sender, "SET 20 FAIL");
            return ESP_ERR_INVALID_ARG;
        }

        bool old_temp_low  = app_logic_is_temp_low_trigger_now();
        bool old_temp_high = app_logic_is_temp_high_trigger_now();

        cfg.ntc_low_limit_c = low;
        cfg.ntc_high_limit_c = high;
        cfg.ntc_calib_c = calib;

        esp_err_t err = runtime_config_save(&cfg);
        modem_service_send_sms(sender, err == ESP_OK ? "SET 20 OK" : "SET 20 FAIL");

        if (err == ESP_OK) {
            app_logic_load_runtime_config();
            
            config_sync_publish_current_config_for_web("sms");

            *ctx.temp_limit_sms_recent = true;
            *ctx.temp_limit_sms_recent_ms = esp_timer_get_time() / 1000;
            app_logic_reset_pending_mqtt_temp_limit();

            bool new_temp_low  = app_logic_is_temp_low_trigger_now();
            bool new_temp_high = app_logic_is_temp_high_trigger_now();

            bool had_temp_alarm = old_temp_low || old_temp_high;
            bool has_temp_alarm = new_temp_low || new_temp_high;

            bool changed_alarm_type =
                (old_temp_low  != new_temp_low) ||
                (old_temp_high != new_temp_high);

            if ((!had_temp_alarm && has_temp_alarm) ||
                (had_temp_alarm && has_temp_alarm && changed_alarm_type)) {
                *ctx.alarm_temp_was_active = false;
                *ctx.temp_low_alarm_active = false;
                *ctx.temp_high_alarm_active = false;
                app_logic_trigger_alarm_immediately_after_limit_update(true, false);
            }
        }

        return err;
    }

    if (strcmp(f[cmd_index], "21") == 0) {
        int low_idx = cmd_index + 1;
        int high_idx = cmd_index + 2;
        int calib_idx = cmd_index + 3;

        if (n <= calib_idx) {
            modem_service_send_sms(sender, "SET 21 FAIL");
            return ESP_ERR_INVALID_ARG;
        }

        float low = strtof(f[low_idx], NULL);
        float high = strtof(f[high_idx], NULL);
        float calib = strtof(f[calib_idx], NULL);

        if (low < 0.0f || low > 100.0f ||
            high < 0.0f || high > 100.0f ||
            low >= high ||
            calib < -20.0f || calib > 20.0f) {
            modem_service_send_sms(sender, "SET 21 FAIL");
            return ESP_ERR_INVALID_ARG;
        }

        bool old_hum_low  = app_logic_is_hum_low_trigger_now();
        bool old_hum_high = app_logic_is_hum_high_trigger_now();

        cfg.hum_low_limit_pct = low;
        cfg.hum_high_limit_pct = high;
        cfg.hum_calib_pct = calib;

        esp_err_t err = runtime_config_save(&cfg);
        modem_service_send_sms(sender, err == ESP_OK ? "SET 21 OK" : "SET 21 FAIL");

        if (err == ESP_OK) {
            app_logic_load_runtime_config();
            config_sync_publish_current_config_for_web("sms");
            *ctx.hum_limit_sms_recent = true;
            *ctx.hum_limit_sms_recent_ms = esp_timer_get_time() / 1000;
            app_logic_reset_pending_mqtt_hum_limit();

            bool new_hum_low  = app_logic_is_hum_low_trigger_now();
            bool new_hum_high = app_logic_is_hum_high_trigger_now();

            bool had_hum_alarm = old_hum_low || old_hum_high;
            bool has_hum_alarm = new_hum_low || new_hum_high;

            bool changed_alarm_type =
                (old_hum_low  != new_hum_low) ||
                (old_hum_high != new_hum_high);

            if ((!had_hum_alarm && has_hum_alarm) ||
                (had_hum_alarm && has_hum_alarm && changed_alarm_type)) {
                *ctx.alarm_hum_was_active = false;
                *ctx.hum_low_alarm_active = false;
                *ctx.hum_high_alarm_active = false;
                app_logic_trigger_alarm_immediately_after_limit_update(false, true);
            }
        }
        return err;
    }

    if (strcmp(f[cmd_index], "HUM_OFF") == 0) {
        bool was_on = cfg.hum_enabled;

        cfg.hum_enabled = false;

        esp_err_t err = runtime_config_save(&cfg);
        if (err != ESP_OK) {
            modem_service_send_sms(sender, "SET HUM OFF FAIL");
            return err;
        }

        app_logic_load_runtime_config();

        /* Xoa ngay state alarm humidity trong RAM, khong doi vong loop */
        if (was_on) {
            app_logic_handle_web_limit_update(cfg.ntc_low_limit_c,
                                            cfg.ntc_high_limit_c,
                                            cfg.hum_low_limit_pct,
                                            cfg.hum_high_limit_pct);
        }

        modem_service_send_sms(sender, "SET HUM OFF OK");
        return ESP_OK;
    }
    if (strcmp(f[cmd_index], "HUM_ON") == 0) {
        cfg.hum_enabled = true;

        esp_err_t err = runtime_config_save(&cfg);
        if (err != ESP_OK) {
            modem_service_send_sms(sender, "SET HUM ON FAIL");
            return err;
        }

        app_logic_load_runtime_config();
        config_sync_publish_current_config_for_web("sms");

        modem_service_send_sms(sender, "SET HUM ON OK");
        return ESP_OK;
    }
    modem_service_send_sms(sender, "SET UNKNOWN");
    return ESP_ERR_NOT_SUPPORTED;
}

