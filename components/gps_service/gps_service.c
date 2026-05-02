#include "gps_service.h"
#include "modem_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "gps_service";

#ifndef GPS_AT_TIMEOUT_MS
#define GPS_AT_TIMEOUT_MS 3000
#endif

#ifndef GPS_FIX_POLL_MS
#define GPS_FIX_POLL_MS 5000
#endif

#ifndef GPS_START_WAIT_MS
#define GPS_START_WAIT_MS 1000
#endif

static SemaphoreHandle_t s_gps_lock;
static bool s_gps_started;
static gps_location_t s_last;

static bool resp_has_ok(const char *s)
{
    return s && strstr(s, "OK") != NULL;
}

static bool resp_has_error(const char *s)
{
    return s && (strstr(s, "ERROR") != NULL || strstr(s, "+CME ERROR") != NULL);
}

static void trim_left(char **p)
{
    while (p && *p && **p && isspace((unsigned char)**p)) {
        (*p)++;
    }
}

static esp_err_t parse_qgpsloc_2(const char *resp, gps_location_t *out)
{
    if (!resp || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = strstr(resp, "+QGPSLOC:");
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }

    p = strchr(p, ':');
    if (!p) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    p++;

    char line[256] = {0};
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < sizeof(line) - 1) {
        line[i++] = *p++;
    }
    line[i] = 0;

    char *fields[16] = {0};
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(line, ",", &save);

    while (tok && n < (int)(sizeof(fields) / sizeof(fields[0]))) {
        trim_left(&tok);
        fields[n++] = tok;
        tok = strtok_r(NULL, ",", &save);
    }

    /*
     * AT+QGPSLOC=2 thường trả về:
     * +QGPSLOC: <UTC>,<lat>,<lon>,<hdop>,<altitude>,<fix>,
     *           <cog>,<spkm>,<spkn>,<date>,<nsat>
     */
    if (n < 11) {
        ESP_LOGW(TAG, "QGPSLOC fields too few: %d resp=[%s]", n, resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    gps_location_t loc = {0};

    snprintf(loc.utc_time, sizeof(loc.utc_time), "%s", fields[0] ? fields[0] : "");
    loc.latitude    = fields[1] ? strtod(fields[1], NULL) : 0.0;
    loc.longitude   = fields[2] ? strtod(fields[2], NULL) : 0.0;
    loc.hdop        = fields[3] ? strtof(fields[3], NULL) : 0.0f;
    loc.altitude_m  = fields[4] ? strtof(fields[4], NULL) : 0.0f;
    loc.fix         = fields[5] ? atoi(fields[5]) : 0;
    loc.speed_kmh   = fields[7] ? strtof(fields[7], NULL) : 0.0f;
    snprintf(loc.utc_date, sizeof(loc.utc_date), "%s", fields[9] ? fields[9] : "");
    loc.satellites  = fields[10] ? atoi(fields[10]) : 0;

    loc.valid = (loc.fix >= 2 && loc.latitude != 0.0 && loc.longitude != 0.0);

    *out = loc;

    if (!loc.valid) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t gps_service_init(void)
{
    if (!s_gps_lock) {
        s_gps_lock = xSemaphoreCreateMutex();
        if (!s_gps_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_last, 0, sizeof(s_last));
    s_gps_started = false;

    return ESP_OK;
}

esp_err_t gps_service_start(void)
{
    char resp[512] = {0};

    if (!s_gps_lock) {
        ESP_RETURN_ON_ERROR(gps_service_init(), TAG, "init failed");
    }

    if (xSemaphoreTake(s_gps_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Tắt echo để response dễ parse.
     */
    (void)modem_service_at_command("ATE0", resp, sizeof(resp), 1000);

    /*
     * Cấu hình GNSS.
     * Nếu firmware không hỗ trợ gnssconfig thì bỏ qua, vẫn thử AT+QGPS=1.
     */
    memset(resp, 0, sizeof(resp));
    (void)modem_service_at_command(
        "AT+QGPSCFG=\"gnssconfig\",1",
        resp,
        sizeof(resp),
        GPS_AT_TIMEOUT_MS
    );

    memset(resp, 0, sizeof(resp));
    esp_err_t err = modem_service_at_command(
        "AT+QGPS=1",
        resp,
        sizeof(resp),
        GPS_AT_TIMEOUT_MS
    );

    if (err == ESP_OK || resp_has_ok(resp)) {
        s_gps_started = true;
        err = ESP_OK;
        ESP_LOGI(TAG, "GNSS started");
        vTaskDelay(pdMS_TO_TICKS(GPS_START_WAIT_MS));
    } else {
        ESP_LOGW(TAG, "GNSS start failed: %s resp=[%s]", esp_err_to_name(err), resp);
    }

    xSemaphoreGive(s_gps_lock);
    return err;
}

esp_err_t gps_service_stop(void)
{
    char resp[256] = {0};

    if (!s_gps_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_gps_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = modem_service_at_command(
        "AT+QGPS=0",
        resp,
        sizeof(resp),
        GPS_AT_TIMEOUT_MS
    );

    if (err != ESP_OK || resp_has_error(resp)) {
        memset(resp, 0, sizeof(resp));
        err = modem_service_at_command(
            "AT+QGPSEND",
            resp,
            sizeof(resp),
            GPS_AT_TIMEOUT_MS
        );
    }

    if (err == ESP_OK || resp_has_ok(resp)) {
        s_gps_started = false;
        err = ESP_OK;
        ESP_LOGI(TAG, "GNSS stopped");
    } else {
        ESP_LOGW(TAG, "GNSS stop failed: %s resp=[%s]", esp_err_to_name(err), resp);
    }

    xSemaphoreGive(s_gps_lock);
    return err;
}

esp_err_t gps_service_read(gps_location_t *out)
{
    char resp[768] = {0};

    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_gps_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_gps_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_gps_started) {
        xSemaphoreGive(s_gps_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = modem_service_at_command(
        "AT+QGPSLOC=2",
        resp,
        sizeof(resp),
        10000
    );

    if (err == ESP_OK) {
        err = parse_qgpsloc_2(resp, out);

        if (err == ESP_OK && out->valid) {
            s_last = *out;

            ESP_LOGI(TAG,
                     "GPS fix lat=%.6f lon=%.6f fix=%d sat=%d hdop=%.1f",
                     out->latitude,
                     out->longitude,
                     out->fix,
                     out->satellites,
                     out->hdop);
        } else {
            ESP_LOGW(TAG,
                     "GPS no fix/parse fail: %s resp=[%s]",
                     esp_err_to_name(err),
                     resp);
        }
    } else {
        ESP_LOGW(TAG,
                 "QGPSLOC failed: %s resp=[%s]",
                 esp_err_to_name(err),
                 resp);
    }

    xSemaphoreGive(s_gps_lock);
    return err;
}

esp_err_t gps_service_get_location_once(gps_location_t *out, uint32_t timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_FAIL;
    uint32_t elapsed = 0;

    /*
     * Modem đang chạy PPP DATA mode.
     * Muốn đọc GPS bằng AT command thì tạm dừng PPP một lần,
     * lấy GPS xong rồi bật lại PPP.
     */
    esp_err_t sess_err = modem_service_begin_command_session("gps");
    if (sess_err != ESP_OK) {
        return sess_err;
    }

    result = gps_service_start();
    if (result != ESP_OK) {
        goto out;
    }

    while (elapsed <= timeout_ms) {
        gps_location_t loc = {0};

        result = gps_service_read(&loc);
        if (result == ESP_OK && loc.valid) {
            *out = loc;
            goto out;
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_FIX_POLL_MS));
        elapsed += GPS_FIX_POLL_MS;
    }

    result = ESP_ERR_TIMEOUT;

out:
    (void)gps_service_stop();

    return modem_service_end_command_session("gps", result);
}

bool gps_service_last_location(gps_location_t *out)
{
    if (!out || !s_last.valid) {
        return false;
    }

    *out = s_last;
    return true;
}

const char *gps_service_err_to_str(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ok";

    case ESP_ERR_TIMEOUT:
        return "timeout_no_fix";

    case ESP_ERR_NOT_FOUND:
        return "no_qgpsloc";

    case ESP_ERR_INVALID_STATE:
        return "gps_not_fixed_or_not_started";

    case ESP_ERR_INVALID_RESPONSE:
        return "invalid_response";

    default:
        return esp_err_to_name(err);
    }
}