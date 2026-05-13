#include "string.h"
#include "app_logic.h"
#include "app_logic_ctx.h"
#include "sms_command.h"
#include "config_sync.h"

#include "app_config.h"
#include "app_events.h"
#include "board.h"
#include "diag_service.h"
#include "modem_service.h"
#include "mqtt_service.h"
#include "ntc_driver.h"
#include "runtime_config.h"
#include "sht30.h"
#include "tm1638.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tm1638_wifi_ui.h"
#include "power_monitor.h"
#include "esp_mac.h"

#include "tm1638_server_ui.h"
#include "ds1307.h"
#include "time_sync.h"

#include <time.h>
#include <sys/time.h>
#include  "tm1638_4G_ui.h"

static const char *TAG = "app_logic";

static volatile int s_pub_period_ms = APP_DEFAULT_PUB_MS;
static TaskHandle_t s_publish_task = NULL;
static TaskHandle_t s_modem_task = NULL;
static QueueHandle_t s_modem_queue = NULL;
static int s_publish_count = 0;
static volatile bool s_uplink_ready = false;
static volatile app_net_type_t s_net_type = APP_NET_NONE;
static volatile bool s_modem_busy = false;

static QueueHandle_t s_cloud_cmd_queue = NULL;
static TaskHandle_t s_cloud_cmd_task = NULL;

static void handle_cmd_topic_cloud_command(const app_cloud_cmd_t *cmd);
static void reply_current_state_internal(const char *request_id, bool ok, const char *extra);
static void handle_reboot_request_internal(void);
static bool s_boot_sensor_ready = false;
typedef enum {
    MODEM_JOB_NONE = 0,
    MODEM_JOB_SMS_TEMP_ALARM,
    MODEM_JOB_SMS_HUMIDITY_ALARM,
    //MODEM_JOB_SMS_ALARM_RESTORED,
    MODEM_JOB_SMS_TEMP_RESTORED,
    MODEM_JOB_SMS_HUM_RESTORED,
    MODEM_JOB_SMS_POWER_LOST,
    MODEM_JOB_SMS_POWER_RESTORED,
    MODEM_JOB_SMS_NTC_FAULT,
    MODEM_JOB_SMS_NTC_RESTORED,
    MODEM_JOB_SMS_HUM_SENSOR_FAULT,
    MODEM_JOB_SMS_HUM_SENSOR_RESTORED,
    MODEM_JOB_SMS_DAILY_HEALTH,
    MODEM_JOB_SMS_MESSAGE1,
    MODEM_JOB_CALL_ALARM,
    MODEM_JOB_CALL_ALARM_RESTORED,
    MODEM_JOB_CALL_POWER_LOST,
    MODEM_JOB_CALL_POWER_RESTORED,
    MODEM_JOB_POLL_INBOX,
} modem_job_type_t;

typedef struct {
    modem_job_type_t type;
} modem_job_t;

typedef struct {
    int total_enabled;
    int ok_count;
    int fail_count;
} sms_send_result_t;


static ntc_t s_ntc1;
static bool s_ntc1_ready = false;
static volatile float s_last_temp1_c = 25.0f;
static volatile bool s_ntc1_valid = false;

static ntc_t s_ntc2;
static bool s_ntc2_ready = false;
static volatile float s_last_temp2_c = 25.0f;
static volatile bool s_ntc2_valid = false;

static sht30_t s_sht30;
static bool s_sht30_ready = false;
static volatile float s_last_sht30_temp_c = 0.0f;
static volatile float s_last_humidity = 0.0f;
static volatile bool s_humidity_valid = false;

static tm1638_t s_tm1638;
static bool s_display_ready = false;

/* Alarm state */
static bool s_alarm_active = false;

static bool s_alarm_sms_pending = false;
static bool s_alarm_sms_sent = false;
static int64_t s_last_sms_try_ms = 0;

static bool s_alarm_call_pending = false;
static bool s_alarm_call_done = false;
static int64_t s_last_call_try_ms = 0;

static bool s_alarm_restored_sms_pending = false;
static bool s_alarm_restored_sms_sent = false;
static int64_t s_last_alarm_restored_sms_try_ms = 0;

static bool s_temp_alarm_sms_job_queued = false;
static bool s_hum_alarm_sms_job_queued = false;
static bool s_alarm_restored_sms_job_queued = false;

static bool s_alarm_restored_call_pending = false;
static bool s_alarm_restored_call_done = false;
static int64_t s_last_alarm_restored_call_try_ms = 0;

static bool s_temp_restored_sms_job_queued = false;
static bool s_hum_restored_sms_job_queued = false;

/* Ghi nho trong mot chu ky alarm: loai nao da tung active */
static bool s_alarm_temp_was_active = false;
static bool s_alarm_hum_was_active = false;

static bool s_temp_low_alarm_active = false;
static bool s_temp_high_alarm_active = false;
static bool s_hum_low_alarm_active = false;
static bool s_hum_high_alarm_active = false;

/* event rieng cho tung kenh */
static bool s_new_temp_alarm_pending = false;
static bool s_new_hum_alarm_pending = false;
static bool s_restore_temp_pending = false;
static bool s_restore_hum_pending = false;

static int64_t s_alarm_assert_start_ms = 0;
static int64_t s_alarm_restore_start_ms = 0;
// static int64_t s_alarm_last_notify_ms = 0;

static int64_t s_temp_alarm_last_notify_ms = 0;
static int64_t s_hum_alarm_last_notify_ms = 0;
static int s_temp_alarm_notify_count = 0;
static int s_hum_alarm_notify_count = 0;

static bool s_power_lost_sms_pending = false;
static bool s_power_lost_sms_sent = false;
static int64_t s_last_power_lost_sms_try_ms = 0;

static bool s_power_restored_sms_pending = false;
static bool s_power_restored_sms_sent = false;
static int64_t s_last_power_restored_sms_try_ms = 0;

static bool s_power_lost_call_pending = false;
static bool s_power_lost_call_done = false;
static int64_t s_last_power_lost_call_try_ms = 0;

static bool s_power_restored_call_pending = false;
static bool s_power_restored_call_done = false;
static int64_t s_last_power_restored_call_try_ms = 0;

static int64_t s_power_lost_last_notify_ms = 0;
static bool s_power_boot_checked = false;

static bool s_blink_on = true;
static int64_t s_last_sms_command_poll_ms = 0;

static bool s_ntc_fault_active = false;
static bool s_sht_fault_active = false;

static bool s_ntc_fault_notify_pending = false;
static bool s_sht_fault_notify_pending = false;

static char s_ntc_fault_reason[32] = "none";
static char s_sht_fault_reason[32] = "none";

static bool s_ntc_fault_sms_pending = false;
static bool s_ntc_fault_sms_sent = false;
static int64_t s_last_ntc_fault_sms_try_ms = 0;
static bool s_ntc_fault_sms_job_queued = false;

static bool s_ntc_restored_sms_pending = false;
static bool s_ntc_restored_sms_sent = false;
static int64_t s_last_ntc_restored_sms_try_ms = 0;
static bool s_ntc_restored_sms_job_queued = false;

#define APP_NTC_BOOT_GRACE_MS          (10 * 1000)
#define APP_NTC_FAULT_CONFIRM_COUNT    3
#define APP_NTC_RESTORE_CONFIRM_COUNT  2

static bool s_ntc_fault_confirmed = false;
static bool s_ntc_fault_sms_sent_once = false;
static uint8_t s_ntc_fault_confirm_count = 0;
static uint8_t s_ntc_restore_confirm_count = 0;

static bool s_sht_fault_confirmed = false;
static bool s_sht_fault_sms_sent_once = false;
static uint8_t s_sht_fault_confirm_count = 0;
static uint8_t s_sht_restore_confirm_count = 0;

static bool s_sht_fault_sms_pending = false;
static bool s_sht_fault_sms_sent = false;
static int64_t s_last_sht_fault_sms_try_ms = 0;
static bool s_sht_fault_sms_job_queued = false;

static bool s_sht_restored_sms_pending = false;
static bool s_sht_restored_sms_sent = false;
static int64_t s_last_sht_restored_sms_try_ms = 0;
static bool s_sht_restored_sms_job_queued = false;

static runtime_config_t s_runtime_cfg;

static void process_alarm_logic(void);
static void process_alarm_sms(void);
static void process_alarm_call(void);
static void process_alarm_restored_sms(void);
static void process_alarm_restored_call(void);
static void process_alarm_reminder(void);
static sms_send_result_t send_alarm_sms_to_configured_numbers(const char *sms);
static bool is_temp_low_trigger_now_internal(void);
static bool is_temp_high_trigger_now_internal(void);
static bool is_hum_low_trigger_now_internal(void);
static bool is_hum_high_trigger_now_internal(void);
static bool is_temp_low_restored_with_hyst(void);
static bool is_temp_high_restored_with_hyst(void);
static bool is_hum_low_restored_with_hyst(void);
static bool is_hum_high_restored_with_hyst(void);
static void build_temp_alarm_sms(char *out, size_t out_len);
static void build_hum_alarm_sms(char *out, size_t out_len);
static void build_alarm_restored_sms(char *out, size_t out_len);
static void build_ntc_fault_sms(char *out, size_t out_len);
static void build_ntc_restored_sms(char *out, size_t out_len);
static void build_hum_sensor_fault_sms(char *out, size_t out_len);
static void build_hum_sensor_restored_sms(char *out, size_t out_len);
static void trigger_alarm_immediately_after_limit_update_internal(bool temp_limit_changed, bool hum_limit_changed);
static void check_new_alarm_causes_and_queue_notify(void);
static void check_individual_restore_and_queue_sms(void);

static void process_ntc_fault_sms(void);
static void process_ntc_restored_sms(void);
static void process_sht_fault_sms(void);
static void process_sht_restored_sms(void);
static void process_power_lost_sms(void);
static void process_power_restored_sms(void);
static void process_power_lost_call(void);
static void process_power_restored_call(void);
static void process_power_lost_reminder(void);
static void build_power_lost_sms(char *out, size_t out_len);
static void build_power_restored_sms(char *out, size_t out_len);
static void on_power_monitor_event(const power_event_t *event, void *user_ctx);

static void modem_task(void *arg);
static bool enqueue_modem_job(modem_job_type_t type);
static void build_sms_for_job(modem_job_type_t type, char *out, size_t out_len);
static void process_incoming_sms_commands(void);

void app_logic_handle_web_limit_update(float old_ntc_low,
                                       float old_ntc_high,
                                       float old_hum_low,
                                       float old_hum_high);

static bool system_time_is_valid(void);
static esp_err_t sync_system_time_from_ds1307_once(void);

static TickType_t s_last_sht30_sample = 0;
#define APP_SHT30_SAMPLE_MS 2000

#define APP_SMS_RETRY_MS               60000
#define APP_ALARM_CALL_RETRY_MS        60000
#define APP_ALARM_CALL_DURATION_MS     7000
#define APP_ALARM_REMIND_MS            (10 * 60 * 1000)
#define APP_POWER_LOST_REMIND_MS       (10 * 60 * 1000)

// Chong nhieu SHT30
#define APP_SHT_FAULT_CONFIRM_COUNT    3
#define APP_SHT_RESTORE_CONFIRM_COUNT  2

#define APP_SENSOR_SAMPLE_MS           500
#define APP_DISPLAY_REFRESH_MS         250
#define APP_BLINK_MS                   500

#define APP_TEMP_HYST_C   0.5f
#define APP_HUM_HYST_PCT  1.5f

#define APP_ALARM_ASSERT_MS            5000
#define APP_ALARM_RESTORE_MS           2500
#define APP_SMS_MQTT_LIMIT_CONFIRM_WINDOW_MS   (30 * 1000)
#define APP_SMS_MQTT_LIMIT_CONFIRM_COUNT       3

// static int s_alarm_notify_count = 0; 
 static int s_power_lost_notify_count = 0;

static int64_t s_last_set_config_ms = 0;
static char s_last_set_config_payload[1024] = {0};

static bool s_temp_limit_sms_recent = false;
static int64_t s_temp_limit_sms_recent_ms = 0;
static bool s_hum_limit_sms_recent = false;
static int64_t s_hum_limit_sms_recent_ms = 0;

static bool s_pending_mqtt_temp_limit_valid = false;
static float s_pending_mqtt_temp_low = 0.0f;
static float s_pending_mqtt_temp_high = 0.0f;
static int s_pending_mqtt_temp_confirm_count = 0;
static int64_t s_pending_mqtt_temp_first_ms = 0;

static bool s_pending_mqtt_hum_limit_valid = false;
static float s_pending_mqtt_hum_low = 0.0f;
static float s_pending_mqtt_hum_high = 0.0f;
static int s_pending_mqtt_hum_confirm_count = 0;
static int64_t s_pending_mqtt_hum_first_ms = 0;

static void reset_pending_mqtt_temp_limit_internal(void);
static void reset_pending_mqtt_hum_limit_internal(void);
static bool mqtt_temp_limit_should_wait_for_confirm_internal(float low, float high, int64_t now_ms);
static bool mqtt_hum_limit_should_wait_for_confirm_internal(float low, float high, int64_t now_ms);
static bool register_pending_mqtt_temp_limit_internal(float low, float high, int64_t now_ms);
static bool register_pending_mqtt_hum_limit_internal(float low, float high, int64_t now_ms);

static bool s_alarm_call_job_queued = false;
static uint8_t s_alarm_call_request_count = 0;

#define TM1638_SIM_SIGNAL_LED_INDEX   6
#define SIM_SIGNAL_SAMPLE_MS          30000
#define SIM_SIGNAL_LED_RSSI_MIN       13

static ds1307_time_t s_rtc_time;
static bool s_rtc_valid = false;
static TickType_t s_last_rtc_read = 0;
static bool s_rtc_synced_from_system = false;
static int s_last_rtc_sync_day = -1;

#define APP_RTC_READ_MS 1000
static void read_rtc_time(void);

/** Bien gui thong bao 7h hang ngay */
static bool s_daily_health_sms_pending = false;
static bool s_daily_health_sms_sent = false;
static bool s_daily_health_sms_job_queued = false;
static int s_daily_health_last_day = -1;
static int s_daily_health_last_month = -1;
static int s_daily_health_last_year = -1;
static int64_t s_last_daily_health_sms_try_ms = 0;

static void process_daily_health_sms(void);
static void build_daily_health_sms(char *out, size_t out_len);
static bool is_daily_health_target_enabled(void);
static void sync_system_time_to_ds1307_once(void);

static TickType_t s_last_rtc_sync_check = 0;

#define APP_SHT30_SAMPLE_MS      2000
#define APP_RTC_SYNC_CHECK_MS   10000

#define APP_TM1638_BRIGHTNESS_MAIN_OK    7
#define APP_TM1638_BRIGHTNESS_MAIN_LOST  1
static uint8_t s_last_display_brightness = 0xFF;

void app_logic_handle_hum_disabled(void);

static void arm_alarm_call_requests(uint8_t count)
{
    if (count == 0) {
        return;
    }

    if ((uint16_t)s_alarm_call_request_count + count > 10) {
        s_alarm_call_request_count = 10;
    } else {
        s_alarm_call_request_count += count;
    }

    s_alarm_call_pending = true;
    s_alarm_call_done = false;
    s_last_call_try_ms = 0;
}


static void cloud_cmd_task(void *arg)
{
    (void)arg;
    app_cloud_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cloud_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            handle_cmd_topic_cloud_command(&cmd);
        }
    }
}
static void handle_cmd_topic_cloud_command(const app_cloud_cmd_t *cmd)
{
    if (cmd == NULL || cmd->cmd[0] == '\0') {
        return;
    }

    ESP_LOGW(TAG, "handle cmd topic: cmd=%s params=%s request_id=%s",
             cmd->cmd,
             cmd->params,
             cmd->request_id);

    /* lenh config tu web */
    if (strcmp(cmd->cmd, "set config") == 0 ||
        strcmp(cmd->cmd, "admin set config") == 0 ||
        strcmp(cmd->cmd, "get config") == 0) {
        config_sync_handle_cloud_command(cmd);
        return;
    }

    /* reset thiet bi */
    if (strcmp(cmd->cmd, "RESET") == 0) {
        reply_current_state_internal(cmd->request_id,
                                     true,
                                     "\"cmd\":\"RESET\",\"msg\":\"rebooting\"");
        vTaskDelay(pdMS_TO_TICKS(1000));
        handle_reboot_request_internal();
        return;
    }

    /* trigger gui SMS bang noi dung message da luu trong config */
    if (strcmp(cmd->cmd, "Message1") == 0) {
        if (!s_runtime_cfg.message[0]) {
            reply_current_state_internal(cmd->request_id,
                                        false,
                                        "\"cmd\":\"Message1\",\"msg\":\"empty_message\"");
            return;
        }

        if (s_modem_busy) {
            reply_current_state_internal(cmd->request_id,
                                        false,
                                        "\"cmd\":\"Message1\",\"msg\":\"modem_busy\"");
            return;
        }

        if (enqueue_modem_job(MODEM_JOB_SMS_MESSAGE1)) {
            reply_current_state_internal(cmd->request_id,
                                        true,
                                        "\"cmd\":\"Message1\",\"msg\":\"queued\"");
        } else {
            reply_current_state_internal(cmd->request_id,
                                        false,
                                        "\"cmd\":\"Message1\",\"msg\":\"queue_full\"");
        }
        return;
    }
    // if (strcmp(cmd->cmd, "Message1") == 0) {
    //     if (!s_runtime_cfg.message[0]) {
    //         reply_current_state_internal(cmd->request_id,
    //                                      false,
    //                                      "\"cmd\":\"Message1\",\"msg\":\"empty_message\"");
    //         return;
    //     }

    //     sms_send_result_t sms_res = send_alarm_sms_to_configured_numbers(s_runtime_cfg.message);

    //     char extra[160];
    //     snprintf(extra, sizeof(extra),
    //              "\"cmd\":\"Message1\",\"total\":%d,\"ok_count\":%d,\"fail_count\":%d",
    //              sms_res.total_enabled,
    //              sms_res.ok_count,
    //              sms_res.fail_count);

    //     reply_current_state_internal(cmd->request_id,
    //                                  (sms_res.total_enabled > 0 && sms_res.fail_count == 0),
    //                                  extra);
    //     return;
    // }

    {
        char extra[128];
        snprintf(extra, sizeof(extra),
                 "\"cmd\":\"%s\",\"msg\":\"unsupported_cmd\"",
                 cmd->cmd);
        reply_current_state_internal(cmd->request_id, false, extra);
    }
}

static void app_logic_load_runtime_config_internal(void)
{
    if (runtime_config_load(&s_runtime_cfg) != ESP_OK) {
        memset(&s_runtime_cfg, 0, sizeof(s_runtime_cfg));
    }

    if (s_runtime_cfg.telemetry_interval_ms < APP_MIN_PUB_MS ||
        s_runtime_cfg.telemetry_interval_ms > APP_MAX_PUB_MS) {
        s_runtime_cfg.telemetry_interval_ms = APP_DEFAULT_PUB_MS;
    }
}

static bool is_placeholder_alarm_number(const char *s)
{
    return s && strcmp(s, "0123456789") == 0;
}

static bool is_valid_alarm_phone_field(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }

    if (is_placeholder_alarm_number(s)) {
        return true;
    }

    int n = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        s++;
        n++;
    }

    return n == 10;
}

static bool alarm_number_enabled(const char *number)
{
    if (!number || !number[0]) {
        return false;
    }

    if (is_placeholder_alarm_number(number)) {
        return false;
    }

    return is_valid_alarm_phone_field(number);
}

static bool has_any_alarm_sms_target(void)
{
    const char *numbers[] = {
        s_runtime_cfg.alarm_number1,
        s_runtime_cfg.alarm_number2,
        s_runtime_cfg.alarm_number3,
        s_runtime_cfg.alarm_number4,
        s_runtime_cfg.alarm_number5,
        s_runtime_cfg.alarm_number6
    };

    for (int i = 0; i < 6; i++) {
        if (alarm_number_enabled(numbers[i])) {
            return true;
        }
    }

    return false;
}

static bool has_alarm_call_target(void)
{
    return alarm_number_enabled(s_runtime_cfg.alarm_number1);
}

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

static const char *net_type_to_str_internal(app_net_type_t type)
{
    switch (type) {
    case APP_NET_AP_ONLY:
        return "ap_only";
    case APP_NET_ETH:
        return "eth";
    case APP_NET_WIFI:
        return "wifi";
    case APP_NET_PPP:
        return "ppp";
    default:
        return "none";
    }
}

static bool temp_is_valid(float t)
{
    if (isnan(t) || isinf(t)) {
        return false;
    }
    return (t >= -40.0f && t <= 125.0f);
}

static void display_dash_range(int start_pos, int count, bool on)
{
    if (!s_display_ready) {
        return;
    }

    for (int i = 0; i < count; i++) {
        tm1638_set_digit(&s_tm1638, start_pos + i, on ? -2 : -1, false);
    }
}

static void update_sensor_fault_state(bool *active_flag,
                                      bool *pending_flag,
                                      char *reason_buf,
                                      size_t reason_buf_len,
                                      bool new_active,
                                      const char *new_reason)
{
    if (!reason_buf || reason_buf_len == 0) {
        return;
    }

    if (!new_reason || !new_reason[0]) {
        new_reason = "none";
    }

    bool reason_changed = (strncmp(reason_buf, new_reason, reason_buf_len) != 0);
    bool active_changed = (*active_flag != new_active);

    if (active_changed || (new_active && reason_changed)) {
        *active_flag = new_active;
        *pending_flag = true;
        snprintf(reason_buf, reason_buf_len, "%s", new_reason);
    } else if (!new_active && reason_changed) {
        snprintf(reason_buf, reason_buf_len, "%s", new_reason);
    }
}

static const char *get_power_status_text_internal(void)
{
    power_sample_t power_sample = {0};
    power_state_t power_state = POWER_STATE_UNKNOWN;

    esp_err_t err = power_monitor_get_latest(&power_sample, &power_state);
    if (err == ESP_OK && power_state == POWER_STATE_MAIN_OK) {
        return "CO DIEN";
    }

    return "MAT DIEN";
}

static void update_do1_output_state(void)
{
    power_sample_t sample = {0};
    power_state_t power_state = POWER_STATE_UNKNOWN;
    bool power_lost = false;

    if (power_monitor_get_latest(&sample, &power_state) == ESP_OK) {
        power_lost = (power_state == POWER_STATE_MAIN_LOST);
    }

    /* Co canh bao nhiet/am HOAC mat dien -> bat ca 2 output */
    bool any_alarm = s_alarm_active || power_lost;

    bool do1_should_on = any_alarm;
    bool do2_should_on = any_alarm;

    board_tca_write_pin(APP_DO1_TCA_PIN, do1_should_on);
    board_tca_write_pin(APP_DO2_TCA_PIN, do2_should_on);

    if (s_display_ready) {
        tm1638_set_led(&s_tm1638, 4, do1_should_on);
    }

    ESP_LOGI(TAG, "DO update: alarm=%d power_lost=%d => do1=%d do2=%d",
             s_alarm_active ? 1 : 0,
             power_lost ? 1 : 0,
             do1_should_on ? 1 : 0,
             do2_should_on ? 1 : 0);
}

static void publish_state_internal(const char *reason)
{
    char payload[512];
    diag_snapshot_t snap;
    diag_get_snapshot(&snap);

    const char *power_status = get_power_status_text_internal();

    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"led\":%s,\"relay\":%s,\"interval_ms\":%d,"
             "\"reason\":\"%s\",\"boot_count\":%lu,\"unclean_reset_count\":%lu,"
             "\"uplink\":%d,\"mqtt\":%d,\"net\":\"%s\","
             "\"temp1\":%.2f,\"temp2\":%.2f,"
             "\"sht30_temp_c\":%.2f,\"humidity\":%.2f,\"humidity_valid\":%d,"
             "\"power_status\":\"%s\","
             "\"alarm_active\":%d,\"sms_sent\":%d,\"call_done\":%d}",
             mqtt_service_get_device_key(),
             board_led_get() ? "true" : "false",
             board_relay_get() ? "true" : "false",
             s_pub_period_ms,
             reason ? reason : "na",
             (unsigned long)snap.boot_count,
             (unsigned long)snap.unclean_reset_count,
             s_uplink_ready ? 1 : 0,
             mqtt_service_is_connected() ? 1 : 0,
             net_type_to_str_internal(s_net_type),
             s_last_temp1_c,
             s_last_temp2_c,
             s_last_sht30_temp_c,
             s_last_humidity,
             s_humidity_valid ? 1 : 0,
             power_status,
             s_alarm_active ? 1 : 0,
             s_alarm_sms_sent ? 1 : 0,
             s_alarm_call_done ? 1 : 0);

    mqtt_service_publish_attributes(payload);
}

static void publish_sensor_fault_event(const char *sensor,
                                       bool active,
                                       const char *reason)
{
    if (!s_uplink_ready || !mqtt_service_is_connected()) {
        return;
    }

    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\","
             "\"event\":\"sensor_fault\","
             "\"sensor\":\"%s\","
             "\"active\":%d,"
             "\"reason\":\"%s\","
             "\"ntc1_valid\":%d,"
             "\"ntc2_valid\":%d,"
             "\"sht_valid\":%d,"
             "\"temp1\":%.2f,"
             "\"temp2\":%.2f,"
             "\"sht30_temp_c\":%.2f,"
             "\"humidity\":%.2f,"
             "\"uptime_sec\":%llu}",
             mqtt_service_get_device_key(),
             sensor ? sensor : "unknown",
             active ? 1 : 0,
             reason ? reason : "none",
             s_ntc1_valid ? 1 : 0,
             s_ntc2_valid ? 1 : 0,
             s_humidity_valid ? 1 : 0,
             s_last_temp1_c,
             s_last_temp2_c,
             s_last_sht30_temp_c,
             s_last_humidity,
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));

    int mid = mqtt_service_publish_telemetry(payload);
    ESP_LOGW(TAG, "sensor event mid=%d payload=%s", mid, payload);
}

static void process_sensor_fault_notifications(void)
{
    if (!s_uplink_ready || !mqtt_service_is_connected()) {
        return;
    }

    if (s_ntc_fault_notify_pending) {
        publish_sensor_fault_event("ntc", s_ntc_fault_active, s_ntc_fault_reason);
        s_ntc_fault_notify_pending = false;
    }

    if (s_sht_fault_notify_pending) {
        publish_sensor_fault_event("sht30", s_sht_fault_active, s_sht_fault_reason);
        s_sht_fault_notify_pending = false;
    }
}

static void handle_reboot_request_internal(void)
{
    diag_mark_clean_shutdown();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void handle_factory_reset_request_internal(void)
{
    if (runtime_config_factory_reset() == ESP_OK) {
        diag_mark_clean_shutdown();
        vTaskDelay(pdMS_TO_TICKS(APP_FACTORY_RESET_DELAY_MS));
        esp_restart();
    }
}

static void reply_current_state_internal(const char *request_id, bool ok, const char *extra)
{
    char body[256];

    if (extra && extra[0]) {
        snprintf(body, sizeof(body), "\"ok\":%s,%s", ok ? "true" : "false", extra);
    } else {
        snprintf(body, sizeof(body), "\"ok\":%s", ok ? "true" : "false");
    }

    mqtt_service_publish_reply(request_id, body);
}

static void app_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base != APP_EVENTS) {
        return;
    }

    if (id == APP_EVENT_CLOUD_COMMAND) {
        if (data && s_cloud_cmd_queue) 
        {
            if (xQueueSend(s_cloud_cmd_queue, data, 0) != pdTRUE) 
            {
                ESP_LOGW(TAG, "cloud cmd queue full");
            }
        }
    }
    else if (id == APP_EVENT_MQTT_CONNECTED) {
        tm1638_server_set_state(SERVER_LED_CONNECTED);
        publish_state_internal("mqtt_connected");
    } else if (id == APP_EVENT_MQTT_DISCONNECTED) {
        tm1638_server_set_state(SERVER_LED_ERROR_BLINK);
    }  
    else if (id == APP_EVENT_NET_UP) 
    {
        app_net_status_t *st = (app_net_status_t *)data;

        if (st) {
            s_net_type = st->type;

            if ((st->type == APP_NET_ETH ||
                st->type == APP_NET_WIFI ||
                st->type == APP_NET_PPP) &&
                st->has_ip) {

                s_uplink_ready = true;
                time_sync_start();

                ESP_LOGI(TAG, "uplink ready: net=%s",
                        net_type_to_str_internal(s_net_type));
            } else {
                s_uplink_ready = false;

                ESP_LOGW(TAG, "net up ignored: type=%d has_ip=%d",
                        st->type,
                        st->has_ip ? 1 : 0);
            }
        }
    }
    else if (id == APP_EVENT_NET_DOWN) 
    {
        app_net_status_t *st = (app_net_status_t *)data;
        if (st && st->type == s_net_type) {
            s_uplink_ready = false;
        }
        if (!st) {
            s_uplink_ready = false;
            s_net_type = APP_NET_NONE;
        }

        tm1638_server_set_state(SERVER_LED_ERROR_BLINK);
    }

}

static bool is_temp_low_trigger_now_internal(void)
{
    return s_ntc1_valid && (s_last_temp1_c < s_runtime_cfg.ntc_low_limit_c);
}

static bool is_temp_high_trigger_now_internal(void)
{
    return s_ntc1_valid && (s_last_temp1_c > s_runtime_cfg.ntc_high_limit_c);
}

static bool is_hum_low_trigger_now_internal(void)
{
    return s_runtime_cfg.hum_enabled &&
           s_humidity_valid &&
           (s_last_humidity < s_runtime_cfg.hum_low_limit_pct);
}

static bool is_hum_high_trigger_now_internal(void)
{
    return s_runtime_cfg.hum_enabled &&
           s_humidity_valid &&
           (s_last_humidity > s_runtime_cfg.hum_high_limit_pct);
}

static bool is_temp_low_restored_with_hyst(void)
{
    return s_ntc1_valid && (s_last_temp1_c >= (s_runtime_cfg.ntc_low_limit_c + APP_TEMP_HYST_C));
}

static bool is_temp_high_restored_with_hyst(void)
{
    return s_ntc1_valid && (s_last_temp1_c <= (s_runtime_cfg.ntc_high_limit_c - APP_TEMP_HYST_C));
}

static bool is_hum_low_restored_with_hyst(void)
{
    if (!s_runtime_cfg.hum_enabled) {
        return true;
    }
    return s_humidity_valid &&
           (s_last_humidity >= (s_runtime_cfg.hum_low_limit_pct + APP_HUM_HYST_PCT));
}

static bool is_hum_high_restored_with_hyst(void)
{
    if (!s_runtime_cfg.hum_enabled) {
        return true;
    }
    return s_humidity_valid &&
           (s_last_humidity <= (s_runtime_cfg.hum_high_limit_pct - APP_HUM_HYST_PCT));
}

static void trigger_alarm_immediately_after_limit_update_internal(bool temp_limit_changed, bool hum_limit_changed)
{
    bool temp_low_now  = app_logic_is_temp_low_trigger_now();
    bool temp_high_now = app_logic_is_temp_high_trigger_now();
    bool hum_low_now   = app_logic_is_hum_low_trigger_now();
    bool hum_high_now  = app_logic_is_hum_high_trigger_now();

    bool temp_alarm_now = temp_low_now || temp_high_now;
    bool hum_alarm_now  = hum_low_now || hum_high_now;

    bool new_temp_alarm = false;
    bool new_hum_alarm  = false;
    bool temp_restored  = false;
    bool hum_restored   = false;

    if (temp_limit_changed) {
        if (temp_alarm_now && !s_alarm_temp_was_active) {
            s_alarm_temp_was_active = true;
            s_temp_low_alarm_active = temp_low_now;
            s_temp_high_alarm_active = temp_high_now;
            s_new_temp_alarm_pending = true;
            new_temp_alarm = true;
        } else if (!temp_alarm_now && s_alarm_temp_was_active) {
            s_alarm_temp_was_active = false;
            s_temp_low_alarm_active = false;
            s_temp_high_alarm_active = false;
            s_restore_temp_pending = true;
            temp_restored = true;
        }
    }

    if (hum_limit_changed) {
        if (hum_alarm_now && !s_alarm_hum_was_active) {
            s_alarm_hum_was_active = true;
            s_hum_low_alarm_active = hum_low_now;
            s_hum_high_alarm_active = hum_high_now;
            s_new_hum_alarm_pending = true;
            new_hum_alarm = true;
        } else if (!hum_alarm_now && s_alarm_hum_was_active) {
            s_alarm_hum_was_active = false;
            s_hum_low_alarm_active = false;
            s_hum_high_alarm_active = false;
            s_restore_hum_pending = true;
            hum_restored = true;
        }
    }

    if (!(new_temp_alarm || new_hum_alarm || temp_restored || hum_restored)) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (new_temp_alarm || new_hum_alarm) {
        bool already_alarm_active = s_alarm_active;

        s_alarm_active = true;
        s_alarm_assert_start_ms = 0;
        s_alarm_restore_start_ms = 0;

        update_do1_output_state();

        s_alarm_sms_pending = true;
        s_alarm_sms_sent = false;
        s_last_sms_try_ms = 0;

        uint8_t call_count = 0;
        if (new_temp_alarm) {
            call_count++;
            s_temp_alarm_last_notify_ms = now_ms;
            s_temp_alarm_notify_count = 1;
        }
        if (new_hum_alarm) {
            call_count++;
            s_hum_alarm_last_notify_ms = now_ms;
            s_hum_alarm_notify_count = 1;
        }

        arm_alarm_call_requests(call_count);

        s_alarm_restored_sms_pending = false;
        s_alarm_restored_sms_sent = false;
        s_last_alarm_restored_sms_try_ms = 0;

        s_alarm_restored_call_pending = false;
        s_alarm_restored_call_done = true;
        s_last_alarm_restored_call_try_ms = 0;

        if (already_alarm_active) {
            ESP_LOGW(TAG,
                     "limit update while alarm already active -> queue NEW immediate SMS/CALL temp_new=%d hum_new=%d temp=%.2f hum=%.2f",
                     new_temp_alarm ? 1 : 0,
                     new_hum_alarm ? 1 : 0,
                     s_last_temp1_c,
                     s_last_humidity);
        } else {
            ESP_LOGW(TAG,
                     "limit update -> queue FIRST alarm SMS/CALL temp_new=%d hum_new=%d temp=%.2f hum=%.2f",
                     new_temp_alarm ? 1 : 0,
                     new_hum_alarm ? 1 : 0,
                     s_last_temp1_c,
                     s_last_humidity);
        }
    }

    if (temp_restored || hum_restored) {
        s_alarm_restored_sms_pending = true;
        s_alarm_restored_sms_sent = false;
        s_last_alarm_restored_sms_try_ms = 0;

        s_alarm_restored_call_pending = false;
        s_alarm_restored_call_done = true;
        s_last_alarm_restored_call_try_ms = 0;

        if (!s_alarm_temp_was_active && !s_alarm_hum_was_active) {
            s_alarm_active = false;
            s_alarm_assert_start_ms = 0;

            s_temp_alarm_last_notify_ms = 0;
            s_hum_alarm_last_notify_ms = 0;
            s_temp_alarm_notify_count = 0;
            s_hum_alarm_notify_count = 0;

            s_alarm_sms_pending = false;
            s_alarm_sms_sent = false;
            s_last_sms_try_ms = 0;

            s_alarm_call_pending = false;
            s_alarm_call_done = false;
            s_last_call_try_ms = 0;

            update_do1_output_state();
        }

        ESP_LOGW(TAG,
                 "limit update -> queue RESTORED SMS temp_restored=%d hum_restored=%d temp=%.2f hum=%.2f",
                 temp_restored ? 1 : 0,
                 hum_restored ? 1 : 0,
                 s_last_temp1_c,
                 s_last_humidity);
    }
}

static void reset_pending_mqtt_temp_limit_internal(void)
{
    s_pending_mqtt_temp_limit_valid = false;
    s_pending_mqtt_temp_low = 0.0f;
    s_pending_mqtt_temp_high = 0.0f;
    s_pending_mqtt_temp_confirm_count = 0;
    s_pending_mqtt_temp_first_ms = 0;
}

static void reset_pending_mqtt_hum_limit_internal(void)
{
    s_pending_mqtt_hum_limit_valid = false;
    s_pending_mqtt_hum_low = 0.0f;
    s_pending_mqtt_hum_high = 0.0f;
    s_pending_mqtt_hum_confirm_count = 0;
    s_pending_mqtt_hum_first_ms = 0;
}

static bool mqtt_temp_limit_should_wait_for_confirm_internal(float low, float high, int64_t now_ms)
{
    if (!s_temp_limit_sms_recent) {
        return false;
    }

    if ((now_ms - s_temp_limit_sms_recent_ms) > APP_SMS_MQTT_LIMIT_CONFIRM_WINDOW_MS) {
        s_temp_limit_sms_recent = false;
        reset_pending_mqtt_temp_limit_internal();
        return false;
    }

    bool current_alarm = is_temp_low_trigger_now_internal() || is_temp_high_trigger_now_internal();
    bool new_alarm = s_ntc1_valid && ((s_last_temp1_c < low) || (s_last_temp1_c > high));

    return current_alarm && !new_alarm;
}

static bool mqtt_hum_limit_should_wait_for_confirm_internal(float low, float high, int64_t now_ms)
{
    if (!s_hum_limit_sms_recent) {
        return false;
    }

    if ((now_ms - s_hum_limit_sms_recent_ms) > APP_SMS_MQTT_LIMIT_CONFIRM_WINDOW_MS) {
        s_hum_limit_sms_recent = false;
        reset_pending_mqtt_hum_limit_internal();
        return false;
    }

    bool current_alarm = is_hum_low_trigger_now_internal() || is_hum_high_trigger_now_internal();
    bool new_alarm = s_humidity_valid && ((s_last_humidity < low) || (s_last_humidity > high));

    return current_alarm && !new_alarm;
}

static bool register_pending_mqtt_temp_limit_internal(float low, float high, int64_t now_ms)
{
    if (s_pending_mqtt_temp_limit_valid &&
        fabsf(s_pending_mqtt_temp_low - low) <= 0.01f &&
        fabsf(s_pending_mqtt_temp_high - high) <= 0.01f &&
        (now_ms - s_pending_mqtt_temp_first_ms) <= APP_SMS_MQTT_LIMIT_CONFIRM_WINDOW_MS) {
        s_pending_mqtt_temp_confirm_count++;
    } else {
        s_pending_mqtt_temp_limit_valid = true;
        s_pending_mqtt_temp_low = low;
        s_pending_mqtt_temp_high = high;
        s_pending_mqtt_temp_confirm_count = 1;
        s_pending_mqtt_temp_first_ms = now_ms;
    }

    ESP_LOGW(TAG,
             "temp mqtt limit candidate %.2f..%.2f confirm=%d/%d",
             low,
             high,
             s_pending_mqtt_temp_confirm_count,
             APP_SMS_MQTT_LIMIT_CONFIRM_COUNT);

    if (s_pending_mqtt_temp_confirm_count >= APP_SMS_MQTT_LIMIT_CONFIRM_COUNT) {
        reset_pending_mqtt_temp_limit_internal();
        s_temp_limit_sms_recent = false;
        return true;
    }

    return false;
}

static bool register_pending_mqtt_hum_limit_internal(float low, float high, int64_t now_ms)
{
    if (s_pending_mqtt_hum_limit_valid &&
        fabsf(s_pending_mqtt_hum_low - low) <= 0.01f &&
        fabsf(s_pending_mqtt_hum_high - high) <= 0.01f &&
        (now_ms - s_pending_mqtt_hum_first_ms) <= APP_SMS_MQTT_LIMIT_CONFIRM_WINDOW_MS) {
        s_pending_mqtt_hum_confirm_count++;
    } else {
        s_pending_mqtt_hum_limit_valid = true;
        s_pending_mqtt_hum_low = low;
        s_pending_mqtt_hum_high = high;
        s_pending_mqtt_hum_confirm_count = 1;
        s_pending_mqtt_hum_first_ms = now_ms;
    }

    ESP_LOGW(TAG,
             "hum mqtt limit candidate %.2f..%.2f confirm=%d/%d",
             low,
             high,
             s_pending_mqtt_hum_confirm_count,
             APP_SMS_MQTT_LIMIT_CONFIRM_COUNT);

    if (s_pending_mqtt_hum_confirm_count >= APP_SMS_MQTT_LIMIT_CONFIRM_COUNT) {
        reset_pending_mqtt_hum_limit_internal();
        s_hum_limit_sms_recent = false;
        return true;
    }

    return false;
}

static void check_new_alarm_causes_and_queue_notify(void)
{
    bool temp_low_now  = is_temp_low_trigger_now_internal();
    bool temp_high_now = is_temp_high_trigger_now_internal();
    bool hum_low_now   = is_hum_low_trigger_now_internal();
    bool hum_high_now  = is_hum_high_trigger_now_internal();

    bool temp_alarm_now = temp_low_now || temp_high_now;
    bool hum_alarm_now  = hum_low_now || hum_high_now;

    bool new_temp_alarm = false;
    bool new_hum_alarm  = false;

    if (temp_alarm_now && !s_alarm_temp_was_active) {
        s_alarm_temp_was_active = true;
        s_temp_low_alarm_active = temp_low_now;
        s_temp_high_alarm_active = temp_high_now;
        s_new_temp_alarm_pending = true;
        new_temp_alarm = true;
    }

    if (hum_alarm_now && !s_alarm_hum_was_active) {
        s_alarm_hum_was_active = true;
        s_hum_low_alarm_active = hum_low_now;
        s_hum_high_alarm_active = hum_high_now;
        s_new_hum_alarm_pending = true;
        new_hum_alarm = true;
    }

    if (!(new_temp_alarm || new_hum_alarm)) {
        return;
    }

    s_alarm_sms_pending = true;
    s_alarm_sms_sent = false;
    s_last_sms_try_ms = 0;

    uint8_t call_count = 0;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (new_temp_alarm) {
        call_count++;
        s_temp_alarm_last_notify_ms = now_ms;
        s_temp_alarm_notify_count = 1;
    }

    if (new_hum_alarm) {
        call_count++;
        s_hum_alarm_last_notify_ms = now_ms;
        s_hum_alarm_notify_count = 1;
    }

    arm_alarm_call_requests(call_count);

    s_alarm_restored_sms_pending = false;
    s_alarm_restored_sms_sent = false;
    s_last_alarm_restored_sms_try_ms = 0;

    s_alarm_restored_call_pending = false;
    s_alarm_restored_call_done = true;
    s_last_alarm_restored_call_try_ms = 0;

    ESP_LOGW(TAG,
             "new alarm cause while alarm active -> queue SMS+CALL temp_new=%d hum_new=%d temp_count=%d hum_count=%d temp=%.2f hum=%.2f",
             new_temp_alarm ? 1 : 0,
             new_hum_alarm ? 1 : 0,
             s_temp_alarm_notify_count,
             s_hum_alarm_notify_count,
             s_last_temp1_c,
             s_last_humidity);
}

static void check_individual_restore_and_queue_sms(void)
{
    bool temp_restored = false;
    bool hum_restored = false;

    if (s_alarm_temp_was_active) {
        bool temp_done = false;

        if (s_temp_low_alarm_active) {
            temp_done = is_temp_low_restored_with_hyst();
        } else if (s_temp_high_alarm_active) {
            temp_done = is_temp_high_restored_with_hyst();
        } else {
            temp_done = !is_temp_low_trigger_now_internal() && !is_temp_high_trigger_now_internal();
        }

        if (temp_done) {
            s_alarm_temp_was_active = false;
            s_temp_low_alarm_active = false;
            s_temp_high_alarm_active = false;
            s_restore_temp_pending = true;
            temp_restored = true;

            s_temp_alarm_last_notify_ms = 0;
            s_temp_alarm_notify_count = 0;

            ESP_LOGI(TAG, "temperature restored individually");
        }
    }

    if (s_alarm_hum_was_active) {
        /* HUM_OFF => xoa alarm do am, KHONG queue SMS restored */
        if (!s_runtime_cfg.hum_enabled) {
            s_alarm_hum_was_active = false;
            s_hum_low_alarm_active = false;
            s_hum_high_alarm_active = false;
            s_restore_hum_pending = false;

            s_new_hum_alarm_pending = false;
            s_hum_alarm_sms_job_queued = false;
            s_hum_alarm_last_notify_ms = 0;
            s_hum_alarm_notify_count = 0;

            ESP_LOGI(TAG, "humidity monitoring disabled -> clear humidity alarm without restored SMS");
        } else {
            bool hum_done = false;

            if (s_hum_low_alarm_active) {
                hum_done = is_hum_low_restored_with_hyst();
            } else if (s_hum_high_alarm_active) {
                hum_done = is_hum_high_restored_with_hyst();
            } else {
                hum_done = !is_hum_low_trigger_now_internal() && !is_hum_high_trigger_now_internal();
            }

            if (hum_done) {
                s_alarm_hum_was_active = false;
                s_hum_low_alarm_active = false;
                s_hum_high_alarm_active = false;
                s_restore_hum_pending = true;
                hum_restored = true;

                s_hum_alarm_last_notify_ms = 0;
                s_hum_alarm_notify_count = 0;

                ESP_LOGI(TAG, "humidity restored individually");
            }
        }
    }

    if (!(temp_restored || hum_restored)) {
        if (!s_alarm_temp_was_active && !s_alarm_hum_was_active) {
            s_alarm_active = false;
            s_alarm_assert_start_ms = 0;

            s_temp_alarm_last_notify_ms = 0;
            s_hum_alarm_last_notify_ms = 0;
            s_temp_alarm_notify_count = 0;
            s_hum_alarm_notify_count = 0;

            s_alarm_sms_pending = false;
            s_alarm_sms_sent = false;
            s_last_sms_try_ms = 0;

            s_alarm_call_pending = false;
            s_alarm_call_done = false;
            s_last_call_try_ms = 0;

            update_do1_output_state();
            ESP_LOGI(TAG, "all temp/hum alarms cleared -> update DO1 by alarm/power state");
        }
        return;
    }

    s_alarm_restored_sms_pending = true;
    s_alarm_restored_sms_sent = false;
    s_last_alarm_restored_sms_try_ms = 0;

    s_alarm_restored_call_pending = false;
    s_alarm_restored_call_done = true;
    s_last_alarm_restored_call_try_ms = 0;

    if (!s_alarm_temp_was_active && !s_alarm_hum_was_active) {
        s_alarm_active = false;
        s_alarm_assert_start_ms = 0;

        s_temp_alarm_last_notify_ms = 0;
        s_hum_alarm_last_notify_ms = 0;
        s_temp_alarm_notify_count = 0;
        s_hum_alarm_notify_count = 0;

        s_alarm_sms_pending = false;
        s_alarm_sms_sent = false;
        s_last_sms_try_ms = 0;

        s_alarm_call_pending = false;
        s_alarm_call_done = false;
        s_last_call_try_ms = 0;

        update_do1_output_state();
        ESP_LOGI(TAG, "all temp/hum alarms restored -> update DO1 by alarm/power state");
    }
}


static void build_temp_alarm_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    snprintf(out, out_len,
             "CANH BAO - Thiet Bi: %s; NHIET DO VUOT NGUONG CAI DAT (%.2f, %.2f); %.2f C; (%s).",
             mqtt_service_get_device_key(),
             s_runtime_cfg.ntc_low_limit_c,
             s_runtime_cfg.ntc_high_limit_c,
             s_last_temp1_c,
             time_buf);
}

static void build_hum_alarm_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    snprintf(out, out_len,
             "CANH BAO - Thiet Bi: %s; DO AM VUOT NGUONG CAI DAT (%.2f, %.2f); %.2f%%; (%s).",
             mqtt_service_get_device_key(),
             s_runtime_cfg.hum_low_limit_pct,
             s_runtime_cfg.hum_high_limit_pct,
             s_last_humidity,
             time_buf);
}

static void build_alarm_restored_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    if (s_restore_temp_pending && !s_restore_hum_pending) {
        snprintf(out, out_len,
                 "THONG BAO - Thiet Bi: %s; NHIET DO TRO LAI BINH THUONG (%.2f, %.2f); %.2f C; (%s).",
                 mqtt_service_get_device_key(),
                 s_runtime_cfg.ntc_low_limit_c,
                 s_runtime_cfg.ntc_high_limit_c,
                 s_last_temp1_c,
                 time_buf);
        return;
    }

    if (s_restore_hum_pending && !s_restore_temp_pending) {
        snprintf(out, out_len,
                 "THONG BAO - Thiet Bi: %s; DO AM TRO LAI BINH THUONG (%.2f, %.2f); %.2f%%; (%s).",
                 mqtt_service_get_device_key(),
                 s_runtime_cfg.hum_low_limit_pct,
                 s_runtime_cfg.hum_high_limit_pct,
                 s_last_humidity,
                 time_buf);
        return;
    }

    if (s_restore_temp_pending && s_restore_hum_pending) {
        snprintf(out, out_len,
                 "THONG BAO - Thiet Bi: %s; NHIET DO VA DO AM TRO LAI BINH THUONG; T=%.2f C; H=%.2f%%; (%s).",
                 mqtt_service_get_device_key(),
                 s_last_temp1_c,
                 s_last_humidity,
                 time_buf);
        return;
    }

    snprintf(out, out_len,
             "THONG BAO - Thiet Bi: %s; GIA TRI TRO LAI BINH THUONG; (%s).",
             mqtt_service_get_device_key(),
             time_buf);
}

static void build_temp_restored_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    snprintf(out, out_len,
             "THONG BAO - Thiet Bi: %s; NHIET DO TRO LAI BINH THUONG (%.2f, %.2f); %.2f C; (%s).",
             mqtt_service_get_device_key(),
             s_runtime_cfg.ntc_low_limit_c,
             s_runtime_cfg.ntc_high_limit_c,
             s_last_temp1_c,
             time_buf);
}

static void build_hum_restored_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    snprintf(out, out_len,
             "THONG BAO - Thiet Bi: %s; DO AM TRO LAI BINH THUONG (%.2f, %.2f); %.2f%%; (%s).",
             mqtt_service_get_device_key(),
             s_runtime_cfg.hum_low_limit_pct,
             s_runtime_cfg.hum_high_limit_pct,
             s_last_humidity,
             time_buf);
}

static void build_power_lost_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    float temp_now = s_ntc1_valid ? s_last_temp1_c : 0.0f;
    float hum_now  = (s_runtime_cfg.hum_enabled && s_humidity_valid) ? s_last_humidity : 0.0f;

    snprintf(out, out_len,
             "CANH BAO - Thiet Bi: %s; MAT DIEN; %.2f; %.0f%%; (%s).",
             mqtt_service_get_device_key(),
             temp_now,
             hum_now,
             time_buf);
}

static void build_power_restored_sms(char *out, size_t out_len)
{
    char time_buf[40];
    sms_command_format_time(time_buf, sizeof(time_buf));

    float temp_now = s_ntc1_valid ? s_last_temp1_c : 0.0f;
    float hum_now  = (s_runtime_cfg.hum_enabled && s_humidity_valid) ? s_last_humidity : 0.0f;

    snprintf(out, out_len,
             "THONG BAO - Thiet Bi: %s; CO DIEN TRO LAI; %.2f; %.0f%%; (%s).",
             mqtt_service_get_device_key(),
             temp_now,
             hum_now,
             time_buf);
}

static void build_ntc_fault_sms(char *out, size_t out_len)
{
    char temp_buf[24];
    char hum_buf[24];

    /* Khi NTC loi, van gui gia tri cuoi cung doc duoc */
    snprintf(temp_buf, sizeof(temp_buf), "%.2fC", s_last_temp1_c);

    if (s_runtime_cfg.hum_enabled) {
        snprintf(hum_buf, sizeof(hum_buf), "%.0f%%", s_last_humidity);
    } else {
        snprintf(hum_buf, sizeof(hum_buf), "0%%");
    }

    snprintf(out, out_len,
             "CANH BAO - %s - Cam Bien NTC Khong Phan Hoi (Nhiet do hien tai: %s, Do am hien tai: %s).",
             mqtt_service_get_device_key(),
             temp_buf,
             hum_buf);
}

static void build_ntc_restored_sms(char *out, size_t out_len)
{
    char temp_buf[24];
    char hum_buf[24];

    snprintf(temp_buf, sizeof(temp_buf), "%.2fC", s_last_temp1_c);

    if (s_runtime_cfg.hum_enabled) {
        snprintf(hum_buf, sizeof(hum_buf), "%.0f%%", s_last_humidity);
    } else {
        snprintf(hum_buf, sizeof(hum_buf), "0%%");
    }

    snprintf(out, out_len,
             "THONG BAO - %s - Cam Bien NTC Tro Lai Binh Thuong (Nhiet do hien tai: %s, Do am hien tai: %s).",
             mqtt_service_get_device_key(),
             temp_buf,
             hum_buf);
}

static void build_hum_sensor_fault_sms(char *out, size_t out_len)
{
    char temp_buf[24];
    char hum_buf[24];

    if (s_ntc1_valid) {
        snprintf(temp_buf, sizeof(temp_buf), "%.2fC", s_last_temp1_c);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "N/A");
    }

    if (s_runtime_cfg.hum_enabled) {
        snprintf(hum_buf, sizeof(hum_buf), "%.0f%%", s_last_humidity);
    } else {
        snprintf(hum_buf, sizeof(hum_buf), "0%%");
    }

    snprintf(out, out_len,
             "CANH BAO - %s - Cam Bien Do Am Khong Phan Hoi (Nhiet do hien tai: %s, Do am hien tai: %s).",
             mqtt_service_get_device_key(),
             temp_buf,
             hum_buf);
}

static void build_hum_sensor_restored_sms(char *out, size_t out_len)
{
    char temp_buf[24];
    char hum_buf[24];

    if (s_ntc1_valid) {
        snprintf(temp_buf, sizeof(temp_buf), "%.2fC", s_last_temp1_c);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "N/A");
    }

    if (s_runtime_cfg.hum_enabled) {
        snprintf(hum_buf, sizeof(hum_buf), "%.0f%%", s_last_humidity);
    } else {
        snprintf(hum_buf, sizeof(hum_buf), "0%%");
    }

    snprintf(out, out_len,
             "THONG BAO - %s - Cam Bien Do Am Tro Lai Binh Thuong (Nhiet do hien tai: %s, Do am hien tai: %s).",
             mqtt_service_get_device_key(),
             temp_buf,
             hum_buf);
}

static void build_sms_for_job(modem_job_type_t type, char *out, size_t out_len)
{
    switch (type) {
    case MODEM_JOB_SMS_TEMP_ALARM:
        build_temp_alarm_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_HUMIDITY_ALARM:
        build_hum_alarm_sms(out, out_len);
        break;
    // case MODEM_JOB_SMS_ALARM_RESTORED:
    //     build_alarm_restored_sms(out, out_len);
    //     break;
    case MODEM_JOB_SMS_TEMP_RESTORED:
        build_temp_restored_sms(out, out_len);
        break;

    case MODEM_JOB_SMS_HUM_RESTORED:
        build_hum_restored_sms(out, out_len);
        break;

    case MODEM_JOB_SMS_POWER_LOST:
        build_power_lost_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_POWER_RESTORED:
        build_power_restored_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_NTC_FAULT:
        build_ntc_fault_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_NTC_RESTORED:
        build_ntc_restored_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_HUM_SENSOR_FAULT:
        build_hum_sensor_fault_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_HUM_SENSOR_RESTORED:
        build_hum_sensor_restored_sms(out, out_len);
        break;
    case MODEM_JOB_SMS_MESSAGE1:
        snprintf(out, out_len, "%s", s_runtime_cfg.message);
        break;
    case MODEM_JOB_SMS_DAILY_HEALTH:
        build_daily_health_sms(out, out_len);
        break;
    default:
        snprintf(out, out_len, "[%s] THONG BAO.", mqtt_service_get_device_key());
        break;
    }
}

static bool enqueue_modem_job(modem_job_type_t type)
{
    if (s_modem_queue == NULL || type == MODEM_JOB_NONE) {
        return false;
    }

    modem_job_t job = {
        .type = type
    };

    BaseType_t ok = xQueueSend(s_modem_queue, &job, 0);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "modem queue full, drop job=%d", (int)type);
        return false;
    }

    return true;
}

static void process_alarm_logic(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    bool temp_low_now  = is_temp_low_trigger_now_internal();
    bool temp_high_now = is_temp_high_trigger_now_internal();
    bool hum_low_now   = is_hum_low_trigger_now_internal();
    bool hum_high_now  = is_hum_high_trigger_now_internal();

    bool temp_alarm_now = temp_low_now || temp_high_now;
    bool hum_alarm_now  = hum_low_now || hum_high_now;
    bool any_alarm_now  = temp_alarm_now || hum_alarm_now;

    if (!s_alarm_active) {
        if (any_alarm_now) {
            if (s_alarm_assert_start_ms == 0) {
                s_alarm_assert_start_ms = now_ms;
                ESP_LOGI(TAG, "alarm assert timer start");
            }

            if ((now_ms - s_alarm_assert_start_ms) >= APP_ALARM_ASSERT_MS) {
                s_alarm_active = true;

                s_alarm_temp_was_active = temp_alarm_now;
                s_alarm_hum_was_active = hum_alarm_now;

                s_temp_low_alarm_active = temp_low_now;
                s_temp_high_alarm_active = temp_high_now;
                s_hum_low_alarm_active = hum_low_now;
                s_hum_high_alarm_active = hum_high_now;

                s_new_temp_alarm_pending = temp_alarm_now;
                s_new_hum_alarm_pending = hum_alarm_now;

                s_alarm_assert_start_ms = 0;
                s_alarm_restore_start_ms = 0;

                update_do1_output_state();

                s_alarm_sms_pending = true;
                s_alarm_sms_sent = false;
                s_last_sms_try_ms = 0;

                uint8_t call_count = 0;
                if (temp_alarm_now) {
                    call_count++;
                }
                if (hum_alarm_now) {
                    call_count++;
                }
                arm_alarm_call_requests(call_count);

                s_alarm_restored_sms_pending = false;
                s_alarm_restored_sms_sent = false;
                s_last_alarm_restored_sms_try_ms = 0;

                s_alarm_restored_call_pending = false;
                s_alarm_restored_call_done = true;
                s_last_alarm_restored_call_try_ms = 0;

                if (temp_alarm_now) {
                    s_temp_alarm_last_notify_ms = now_ms;
                    s_temp_alarm_notify_count = 1;
                }
                if (hum_alarm_now) {
                    s_hum_alarm_last_notify_ms = now_ms;
                    s_hum_alarm_notify_count = 1;
                }

                ESP_LOGW(TAG,
                         "alarm active after confirm TL=%d TH=%d HL=%d HH=%d temp=%.2f hum=%.2f DO1=ON",
                         s_temp_low_alarm_active ? 1 : 0,
                         s_temp_high_alarm_active ? 1 : 0,
                         s_hum_low_alarm_active ? 1 : 0,
                         s_hum_high_alarm_active ? 1 : 0,
                         s_last_temp1_c,
                         s_last_humidity);
            }
        } else {
            s_alarm_assert_start_ms = 0;
        }

        return;
    }

    if (temp_low_now) {
        s_temp_low_alarm_active = true;
        s_temp_high_alarm_active = false;
    } else if (temp_high_now) {
        s_temp_high_alarm_active = true;
        s_temp_low_alarm_active = false;
    }

    if (hum_low_now) {
        s_hum_low_alarm_active = true;
        s_hum_high_alarm_active = false;
    } else if (hum_high_now) {
        s_hum_high_alarm_active = true;
        s_hum_low_alarm_active = false;
    }

    check_new_alarm_causes_and_queue_notify();
    check_individual_restore_and_queue_sms();
}

static void process_alarm_sms(void)
{
    if (!s_alarm_sms_pending || s_alarm_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip active alarm SMS");
        s_alarm_sms_pending = false;
        s_alarm_sms_sent = true;
        s_new_temp_alarm_pending = false;
        s_new_hum_alarm_pending = false;
        s_temp_alarm_sms_job_queued = false;
        s_hum_alarm_sms_job_queued = false;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_sms_try_ms != 0 &&
        (now_ms - s_last_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    bool queued = false;

    if (s_new_temp_alarm_pending && !s_temp_alarm_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_TEMP_ALARM)) {
            s_temp_alarm_sms_job_queued = true;
            ESP_LOGW(TAG, "queued temperature alarm SMS");
            queued = true;
        }
    }

    if (s_new_hum_alarm_pending && !s_hum_alarm_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_HUMIDITY_ALARM)) {
            s_hum_alarm_sms_job_queued = true;
            ESP_LOGW(TAG, "queued humidity alarm SMS");
            queued = true;
        }
    }

    if (queued) {
        s_last_sms_try_ms = now_ms;
    }
}

static void process_alarm_restored_sms(void)
{
    if ((!s_restore_temp_pending && !s_restore_hum_pending) || s_alarm_restored_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip restored alarm SMS");
        s_restore_temp_pending = false;
        s_restore_hum_pending = false;
        s_alarm_restored_sms_sent = true;
        s_temp_restored_sms_job_queued = false;
        s_hum_restored_sms_job_queued = false;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_alarm_restored_sms_try_ms != 0 &&
        (now_ms - s_last_alarm_restored_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    bool queued = false;

    if (s_restore_temp_pending && !s_temp_restored_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_TEMP_RESTORED)) {
            s_temp_restored_sms_job_queued = true;
            queued = true;
            ESP_LOGW(TAG, "queued temperature restored SMS");
        }
    }

    if (s_restore_hum_pending && !s_hum_restored_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_HUM_RESTORED)) {
            s_hum_restored_sms_job_queued = true;
            queued = true;
            ESP_LOGW(TAG, "queued humidity restored SMS");
        }
    }

    if (queued) {
        s_last_alarm_restored_sms_try_ms = now_ms;
    }
}

static void process_alarm_call(void)
{
    if (!s_alarm_call_pending || s_alarm_call_done) {
        return;
    }

    if (!has_alarm_call_target()) {
        ESP_LOGW(TAG, "alarm_number1 empty/invalid -> skip active alarm call");
        s_alarm_call_pending = false;
        s_alarm_call_done = true;
        s_alarm_call_job_queued = false;
        s_alarm_call_request_count = 0;
        return;
    }

    if (s_alarm_call_request_count == 0) {
        s_alarm_call_pending = false;
        s_alarm_call_done = true;
        s_alarm_call_job_queued = false;
        return;
    }

    if (s_alarm_call_job_queued) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_call_try_ms) < APP_ALARM_CALL_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_call_try_ms = now_ms;
    s_alarm_call_job_queued = true;

    if (enqueue_modem_job(MODEM_JOB_CALL_ALARM)) {
        ESP_LOGW(TAG, "queued alarm call to number1 remain=%u",
                 (unsigned)s_alarm_call_request_count);
    } else {
        s_alarm_call_job_queued = false;
    }
}

static void process_alarm_restored_call(void)
{
    if (!s_alarm_restored_call_pending || s_alarm_restored_call_done) {
        return;
    }

    if (!has_alarm_call_target()) {
        ESP_LOGW(TAG, "alarm_number1 empty/invalid -> skip restored alarm call");
        s_alarm_restored_call_pending = false;
        s_alarm_restored_call_done = true;
        s_restore_temp_pending = false;
        s_restore_hum_pending = false;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_alarm_restored_call_try_ms) < APP_ALARM_CALL_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_alarm_restored_call_try_ms = now_ms;

    if (enqueue_modem_job(MODEM_JOB_CALL_ALARM_RESTORED)) {
        ESP_LOGW(TAG, "queued alarm restored call to number1");
    }
}

static void process_alarm_reminder(void)
{
    if (!s_alarm_active) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool any_sms = false;
    uint8_t call_count = 0;

    /* Nhac lai rieng cho nhiet: toi da 3 lan tong cong */
    if (s_alarm_temp_was_active &&
        s_temp_alarm_notify_count > 0 &&
        s_temp_alarm_notify_count < 3 &&
        s_temp_alarm_last_notify_ms > 0 &&
        (now_ms - s_temp_alarm_last_notify_ms) >= APP_ALARM_REMIND_MS) {

        s_new_temp_alarm_pending = true;
        any_sms = true;
        call_count++;

        s_temp_alarm_last_notify_ms = now_ms;
        s_temp_alarm_notify_count++;

        ESP_LOGW(TAG, "temperature reminder queued count=%d/3",
                 s_temp_alarm_notify_count);
    }

    /* Nhac lai rieng cho do am: toi da 3 lan tong cong */
    if (s_alarm_hum_was_active &&
        s_hum_alarm_notify_count > 0 &&
        s_hum_alarm_notify_count < 3 &&
        s_hum_alarm_last_notify_ms > 0 &&
        (now_ms - s_hum_alarm_last_notify_ms) >= APP_ALARM_REMIND_MS) {

        s_new_hum_alarm_pending = true;
        any_sms = true;
        call_count++;

        s_hum_alarm_last_notify_ms = now_ms;
        s_hum_alarm_notify_count++;

        ESP_LOGW(TAG, "humidity reminder queued count=%d/3",
                 s_hum_alarm_notify_count);
    }

    if (!any_sms && call_count == 0) {
        return;
    }

    s_alarm_sms_pending = true;
    s_alarm_sms_sent = false;
    s_last_sms_try_ms = 0;

    arm_alarm_call_requests(call_count);
}

static bool same_alarm_phone(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static sms_send_result_t send_alarm_sms_to_configured_numbers(const char *sms)
{
    sms_send_result_t result = {
        .total_enabled = 0,
        .ok_count = 0,
        .fail_count = 0
    };

    ESP_LOGW(TAG, "alarm numbers: n1=[%s] n2=[%s] n3=[%s] n4=[%s] n5=[%s] n6=[%s]",
             s_runtime_cfg.alarm_number1,
             s_runtime_cfg.alarm_number2,
             s_runtime_cfg.alarm_number3,
             s_runtime_cfg.alarm_number4,
             s_runtime_cfg.alarm_number5,
             s_runtime_cfg.alarm_number6);

    const char *numbers[] = {
        s_runtime_cfg.alarm_number1,
        s_runtime_cfg.alarm_number2,
        s_runtime_cfg.alarm_number3,
        s_runtime_cfg.alarm_number4,
        s_runtime_cfg.alarm_number5,
        s_runtime_cfg.alarm_number6
    };

    for (int i = 0; i < 6; i++) {
        const char *num = numbers[i];
        int idx = i + 1;
        bool duplicate = false;

        if (!alarm_number_enabled(num)) {
            ESP_LOGW(TAG, "skip n%d", idx);
            continue;
        }

        /* Kiem tra so trung nhau bo qua */
        // for (int j = 0; j < i; j++) {
        //     if (alarm_number_enabled(numbers[j]) && same_alarm_phone(num, numbers[j])) {
        //         duplicate = true;
        //         break;
        //     }
        // }

        // if (duplicate) {
        //     ESP_LOGW(TAG, "skip n%d duplicate [%s]", idx, num);
        //     continue;
        // }

        result.total_enabled++;

        ESP_LOGW(TAG, "send alarm sms -> n%d [%s]", idx, num);
        if (modem_service_send_sms(num, sms) == ESP_OK) {
            result.ok_count++;
        } else {
            result.fail_count++;
            ESP_LOGE(TAG, "send alarm sms fail -> n%d [%s]", idx, num);
        }
    }

    ESP_LOGW(TAG, "sms result total=%d ok=%d fail=%d",
             result.total_enabled, result.ok_count, result.fail_count);

    return result;
}

static void sample_sensors(void)
{
    /* =========================
     * NTC1
     * ========================= */
    if (s_ntc1_ready) {
        float t1 = ntc_read_temperature(&s_ntc1);
        ntc_status_t ntc1_status = ntc_get_status(&s_ntc1);

        if (ntc1_status == NTC_STATUS_OK && temp_is_valid(t1)) {
            t1 += s_runtime_cfg.ntc_calib_c;

            if (temp_is_valid(t1)) {
                s_last_temp1_c = t1;
                s_ntc1_valid = true;

                update_sensor_fault_state(&s_ntc_fault_active,
                                          &s_ntc_fault_notify_pending,
                                          s_ntc_fault_reason,
                                          sizeof(s_ntc_fault_reason),
                                          false,
                                          "none");
            } else {
                s_ntc1_valid = false;

                update_sensor_fault_state(&s_ntc_fault_active,
                                          &s_ntc_fault_notify_pending,
                                          s_ntc_fault_reason,
                                          sizeof(s_ntc_fault_reason),
                                          true,
                                          "calibrated_temp_invalid");

                ESP_LOGW(TAG, "NTC1 calibrated temp invalid: %.2f", t1);
            }
        } else {
            const char *reason = "invalid_or_disconnected";

            s_ntc1_valid = false;

            switch (ntc1_status) {
            case NTC_STATUS_OPEN_CIRCUIT:
                reason = "open_circuit_or_unplugged";
                break;
            case NTC_STATUS_SHORT_CIRCUIT:
                reason = "short_circuit";
                break;
            case NTC_STATUS_ADC_ERROR:
                reason = "adc_error";
                break;
            case NTC_STATUS_OUT_OF_RANGE:
                reason = "temp_out_of_range";
                break;
            case NTC_STATUS_OK:
            default:
                reason = "invalid_or_disconnected";
                break;
            }

            update_sensor_fault_state(&s_ntc_fault_active,
                                      &s_ntc_fault_notify_pending,
                                      s_ntc_fault_reason,
                                      sizeof(s_ntc_fault_reason),
                                      true,
                                      reason);

            ESP_LOGW(TAG, "NTC1 read invalid, status=%s",
                     ntc_status_to_string(ntc1_status));
        }
    } else {
        s_ntc1_valid = false;

        update_sensor_fault_state(&s_ntc_fault_active,
                                  &s_ntc_fault_notify_pending,
                                  s_ntc_fault_reason,
                                  sizeof(s_ntc_fault_reason),
                                  true,
                                  "not_initialized");
    }

    /* =========================
     * NTC2
     * ========================= */
    if (s_ntc2_ready) {
        float t2 = ntc_read_temperature(&s_ntc2);
        if (temp_is_valid(t2)) {
            s_last_temp2_c = t2;
            s_ntc2_valid = true;
        } else {
            s_ntc2_valid = false;
            ESP_LOGW(TAG, "NTC2 read invalid");
        }
    } else {
        s_ntc2_valid = false;
    }

    /* =========================
     * NTC fault confirm / restore
     * ========================= */
    {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool ntc_fault_now = s_ntc_fault_active;

        if (now_ms < APP_NTC_BOOT_GRACE_MS) {
            /* 10 giay dau boot: khong gui SMS fault/restored NTC */
            s_ntc_fault_confirmed = false;
            s_ntc_fault_sms_sent_once = false;
            s_ntc_fault_confirm_count = 0;
            s_ntc_restore_confirm_count = 0;

            s_ntc_fault_sms_pending = false;
            s_ntc_fault_sms_sent = false;
            s_last_ntc_fault_sms_try_ms = 0;
            s_ntc_fault_sms_job_queued = false;

            s_ntc_restored_sms_pending = false;
            s_ntc_restored_sms_sent = false;
            s_last_ntc_restored_sms_try_ms = 0;
            s_ntc_restored_sms_job_queued = false;
        } else {
            if (ntc_fault_now) {
                s_ntc_restore_confirm_count = 0;

                if (s_ntc_fault_confirm_count < 255) {
                    s_ntc_fault_confirm_count++;
                }

                if (!s_ntc_fault_confirmed &&
                    s_ntc_fault_confirm_count >= APP_NTC_FAULT_CONFIRM_COUNT) {

                    s_ntc_fault_confirmed = true;

                    s_ntc_fault_sms_pending = true;
                    s_ntc_fault_sms_sent = false;
                    s_last_ntc_fault_sms_try_ms = 0;
                    s_ntc_fault_sms_job_queued = false;

                    s_ntc_restored_sms_pending = false;
                    s_ntc_restored_sms_sent = false;
                    s_last_ntc_restored_sms_try_ms = 0;
                    s_ntc_restored_sms_job_queued = false;

                    ESP_LOGW(TAG, "NTC fault confirmed after boot grace -> queue SMS only");
                }
            } else {
                s_ntc_fault_confirm_count = 0;

                if (s_ntc_fault_confirmed) {
                    if (s_ntc_restore_confirm_count < 255) {
                        s_ntc_restore_confirm_count++;
                    }

                    if (s_ntc_restore_confirm_count >= APP_NTC_RESTORE_CONFIRM_COUNT) {
                        s_ntc_fault_confirmed = false;
                        s_ntc_restore_confirm_count = 0;

                        if (s_ntc_fault_sms_sent_once) {
                            s_ntc_restored_sms_pending = true;
                            s_ntc_restored_sms_sent = false;
                            s_last_ntc_restored_sms_try_ms = 0;
                            s_ntc_restored_sms_job_queued = false;

                            s_ntc_fault_sms_pending = false;
                            s_ntc_fault_sms_sent = false;
                            s_last_ntc_fault_sms_try_ms = 0;
                            s_ntc_fault_sms_job_queued = false;

                            ESP_LOGW(TAG, "NTC restored confirmed -> queue SMS only");
                        } else {
                            s_ntc_restored_sms_pending = false;
                            s_ntc_restored_sms_sent = false;
                            s_last_ntc_restored_sms_try_ms = 0;
                            s_ntc_restored_sms_job_queued = false;
                        }
                    }
                } else {
                    s_ntc_restore_confirm_count = 0;
                    s_ntc_restored_sms_pending = false;
                }
            }
        }
    }

    /* =========================
     * SHT30 / Humidity
     * HUM_OFF => khong theo doi loi cam bien do am
     * ========================= */
    if (!s_runtime_cfg.hum_enabled) 
    /* =========================
 * SHT30 / Humidity
 * HUM_OFF => khong theo doi loi cam bien do am
 * Doc cham theo APP_SHT30_SAMPLE_MS de tranh loi I2C thoang qua
 * ========================= */
    {
        TickType_t now_tick = xTaskGetTickCount();
        bool do_sht30_sample =
            ((now_tick - s_last_sht30_sample) >= pdMS_TO_TICKS(APP_SHT30_SAMPLE_MS));

        if (!s_runtime_cfg.hum_enabled) {
            /* Khong dung hum -> khong do moi, khong bao loi cam bien do am */
            s_humidity_valid = false;

            update_sensor_fault_state(&s_sht_fault_active,
                                    &s_sht_fault_notify_pending,
                                    s_sht_fault_reason,
                                    sizeof(s_sht_fault_reason),
                                    false,
                                    "hum_off");
        } else if (!do_sht30_sample) {
            /* Chua den chu ky doc moi -> giu nguyen gia tri/trang thai cu */
        } else if (s_sht30_ready) {
            s_last_sht30_sample = now_tick;

            sht30_status_t status = sht30_single_shot(&s_sht30,
                                                    Repeatability_High,
                                                    ClockStretching_Disable);
            if (status == ok) {
                float t = sht30_read_temperature_celsius(&s_sht30);
                float h = sht30_read_humidity(&s_sht30) + s_runtime_cfg.hum_calib_pct;

                if (h < 0.0f) h = 0.0f;
                if (h > 100.0f) h = 100.0f;

                if (temp_is_valid(t) && !isnan(h) && !isinf(h) && h >= 0.0f && h <= 100.0f) {
                    s_last_sht30_temp_c = t;
                    s_last_humidity = h;
                    s_humidity_valid = true;

                    update_sensor_fault_state(&s_sht_fault_active,
                                            &s_sht_fault_notify_pending,
                                            s_sht_fault_reason,
                                            sizeof(s_sht_fault_reason),
                                            false,
                                            "none");
                } else {
                    s_humidity_valid = false;

                    update_sensor_fault_state(&s_sht_fault_active,
                                            &s_sht_fault_notify_pending,
                                            s_sht_fault_reason,
                                            sizeof(s_sht_fault_reason),
                                            true,
                                            "data_invalid");

                    ESP_LOGW(TAG, "SHT30 data invalid temp=%.2f hum=%.2f keep_last=%.2f",
                            t, h, s_last_humidity);
                }
            } else {
                s_humidity_valid = false;

                update_sensor_fault_state(&s_sht_fault_active,
                                        &s_sht_fault_notify_pending,
                                        s_sht_fault_reason,
                                        sizeof(s_sht_fault_reason),
                                        true,
                                        "read_failed");

                ESP_LOGW(TAG, "SHT30 read failed, status=%d keep_last=%.2f",
                        status, s_last_humidity);
            }
        } else {
            s_humidity_valid = false;

            update_sensor_fault_state(&s_sht_fault_active,
                                    &s_sht_fault_notify_pending,
                                    s_sht_fault_reason,
                                    sizeof(s_sht_fault_reason),
                                    true,
                                    "not_initialized");
        }
    }

    else if (s_sht30_ready) {
        sht30_status_t status = sht30_single_shot(&s_sht30,
                                                  Repeatability_High,
                                                  ClockStretching_Disable);
        if (status == ok) {
            float t = sht30_read_temperature_celsius(&s_sht30);
            float h = sht30_read_humidity(&s_sht30) + s_runtime_cfg.hum_calib_pct;

            if (h < 0.0f) h = 0.0f;
            if (h > 100.0f) h = 100.0f;

            if (temp_is_valid(t) && !isnan(h) && !isinf(h) && h >= 0.0f && h <= 100.0f) {
                s_last_sht30_temp_c = t;
                s_last_humidity = h;
                s_humidity_valid = true;

                update_sensor_fault_state(&s_sht_fault_active,
                                          &s_sht_fault_notify_pending,
                                          s_sht_fault_reason,
                                          sizeof(s_sht_fault_reason),
                                          false,
                                          "none");
            } else {
                s_humidity_valid = false;

                update_sensor_fault_state(&s_sht_fault_active,
                                          &s_sht_fault_notify_pending,
                                          s_sht_fault_reason,
                                          sizeof(s_sht_fault_reason),
                                          true,
                                          "data_invalid");

                ESP_LOGW(TAG, "SHT30 data invalid temp=%.2f hum=%.2f keep_last=%.2f",
                         t, h, s_last_humidity);
            }
        } else {
            s_humidity_valid = false;

            update_sensor_fault_state(&s_sht_fault_active,
                                      &s_sht_fault_notify_pending,
                                      s_sht_fault_reason,
                                      sizeof(s_sht_fault_reason),
                                      true,
                                      "read_failed");

            ESP_LOGW(TAG, "SHT30 read failed, status=%d keep_last=%.2f",
                     status, s_last_humidity);
        }
    } else {
        s_humidity_valid = false;

        update_sensor_fault_state(&s_sht_fault_active,
                                  &s_sht_fault_notify_pending,
                                  s_sht_fault_reason,
                                  sizeof(s_sht_fault_reason),
                                  true,
                                  "not_initialized");
    }
    
    /* =========================
     * SHT30 fault confirm / restore
     * HUM_OFF => xoa toan bo pending/confirm, khong gui canh bao
     * ========================= */
    
        if (!s_runtime_cfg.hum_enabled) {
            s_sht_fault_confirmed = false;
            s_sht_fault_sms_sent_once = false;
            s_sht_fault_confirm_count = 0;
            s_sht_restore_confirm_count = 0;

            s_sht_fault_sms_pending = false;
            s_sht_fault_sms_sent = false;
            s_last_sht_fault_sms_try_ms = 0;
            s_sht_fault_sms_job_queued = false;

            s_sht_restored_sms_pending = false;
            s_sht_restored_sms_sent = false;
            s_last_sht_restored_sms_try_ms = 0;
            s_sht_restored_sms_job_queued = false;

             s_new_hum_alarm_pending = false;
            s_restore_hum_pending = false;
            s_hum_alarm_sms_job_queued = false;
            s_hum_alarm_last_notify_ms = 0;
            s_hum_alarm_notify_count = 0;
        } else {
            bool sht_fault_now = s_sht_fault_active;

            if (sht_fault_now) {
                s_sht_restore_confirm_count = 0;

                if (s_sht_fault_confirm_count < 255) {
                    s_sht_fault_confirm_count++;
                }

                if (!s_sht_fault_confirmed &&
                    s_sht_fault_confirm_count >= APP_SHT_FAULT_CONFIRM_COUNT) {

                    s_sht_fault_confirmed = true;

                    s_sht_fault_sms_pending = true;
                    s_sht_fault_sms_sent = false;
                    s_last_sht_fault_sms_try_ms = 0;
                    s_sht_fault_sms_job_queued = false;

                    s_sht_restored_sms_pending = false;
                    s_sht_restored_sms_sent = false;
                    s_last_sht_restored_sms_try_ms = 0;
                    s_sht_restored_sms_job_queued = false;

                    ESP_LOGW(TAG, "SHT30 fault confirmed -> queue SMS only");
                }
            } else {
                s_sht_fault_confirm_count = 0;

                if (s_sht_fault_confirmed) {
                    if (s_sht_restore_confirm_count < 255) {
                        s_sht_restore_confirm_count++;
                    }

                    // if (s_sht_restore_confirm_count >= APP_SHT_RESTORE_CONFIRM_COUNT) {
                    //     s_sht_fault_confirmed = false;
                    //     s_sht_restore_confirm_count = 0;

                    //     if (s_sht_fault_sms_sent_once) {
                    //         s_sht_restored_sms_pending = true;
                    //         s_sht_restored_sms_sent = false;
                    //         s_last_sht_restored_sms_try_ms = 0;
                    //         s_sht_restored_sms_job_queued = false;

                    //         s_sht_fault_sms_pending = false;
                    //         s_sht_fault_sms_sent = false;
                    //         s_last_sht_fault_sms_try_ms = 0;
                    //         s_sht_fault_sms_job_queued = false;

                    //         ESP_LOGW(TAG, "SHT30 restored confirmed -> queue SMS only");
                    //     } else {
                    //         s_sht_restored_sms_pending = false;
                    //         s_sht_restored_sms_sent = false;
                    //         s_last_sht_restored_sms_try_ms = 0;
                    //         s_sht_restored_sms_job_queued = false;
                    //     }
                    // }
                    if (s_sht_restore_confirm_count >= APP_SHT_RESTORE_CONFIRM_COUNT) {
                        s_sht_fault_confirmed = false;
                        s_sht_restore_confirm_count = 0;

                        /*
                        * Da xac nhan SHT30 fault truoc do.
                        * Khi SHT30 doc OK du so lan restore confirm,
                        * thi queue SMS restored.
                        *
                        * Khong phu thuoc tuyet doi vao s_sht_fault_sms_sent_once,
                        * vi co truong hop SMS fault da queue/gui cham, hoac flag chua kip cap nhat.
                        */
                        s_sht_restored_sms_pending = true;
                        s_sht_restored_sms_sent = false;
                        s_last_sht_restored_sms_try_ms = 0;
                        s_sht_restored_sms_job_queued = false;

                        s_sht_fault_sms_pending = false;
                        s_sht_fault_sms_sent = false;
                        s_last_sht_fault_sms_try_ms = 0;
                        s_sht_fault_sms_job_queued = false;

                        ESP_LOGW(TAG,
                                "SHT30 restored confirmed -> queue SMS restored, sent_once=%d",
                                s_sht_fault_sms_sent_once ? 1 : 0);
                    }
                } else {
                    s_sht_restore_confirm_count = 0;
                    s_sht_restored_sms_pending = false;
                }
            }
        }
}

static void update_display(void)
{
    if (!s_display_ready) {
        return;
    }

    if (s_ntc1_valid) {
        esp_err_t err = tm1638_display_float(&s_tm1638, 0, 4, s_last_temp1_c, 1, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display temp failed: %s", esp_err_to_name(err));
        }
    } else {
        display_dash_range(0, 4, s_blink_on);
    }

    // if (!s_runtime_cfg.hum_enabled) {
    //     display_dash_range(4, 4, false);
    // } else if (s_humidity_valid) {
    //     esp_err_t err = tm1638_display_float(&s_tm1638, 4, 4, s_last_humidity, 1, false);
    //     if (err != ESP_OK) {
    //         ESP_LOGW(TAG, "display humidity failed: %s", esp_err_to_name(err));
    //     }
    // } else {
    //     //display_dash_range(4, 4, s_blink_on);
    //     display_dash_range(4, 4, true);
    // }
    if (!s_runtime_cfg.hum_enabled) {
        display_dash_range(4, 4, false);
    } else if (s_humidity_valid || !s_sht_fault_confirmed) {
        esp_err_t err = tm1638_display_float(&s_tm1638, 4, 4, s_last_humidity, 1, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display humidity failed: %s", esp_err_to_name(err));
        }
    } else {
        display_dash_range(4, 4, true);
    }
}

static void modem_task(void *arg)
{
    (void)arg;
    modem_job_t job;

    while (1) {
        if (xQueueReceive(s_modem_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_modem_busy = true;
        ESP_LOGW(TAG, "modem task start job=%d", (int)job.type);

        switch (job.type) {
        case MODEM_JOB_SMS_TEMP_ALARM:
        case MODEM_JOB_SMS_HUMIDITY_ALARM:
        case MODEM_JOB_SMS_POWER_LOST:
        case MODEM_JOB_SMS_POWER_RESTORED:
        case MODEM_JOB_SMS_NTC_FAULT:
        case MODEM_JOB_SMS_NTC_RESTORED:
        case MODEM_JOB_SMS_HUM_SENSOR_FAULT:
        case MODEM_JOB_SMS_HUM_SENSOR_RESTORED:
        case MODEM_JOB_SMS_TEMP_RESTORED:
        case MODEM_JOB_SMS_HUM_RESTORED: {
            char sms[320];
            build_sms_for_job(job.type, sms, sizeof(sms));

            sms_send_result_t sms_res = send_alarm_sms_to_configured_numbers(sms);
            bool no_target = (sms_res.total_enabled == 0);
            bool all_sent_ok = (sms_res.total_enabled > 0) &&
                               (sms_res.fail_count == 0) &&
                               (sms_res.ok_count == sms_res.total_enabled);
            bool sms_done = all_sent_ok || no_target;

            if (job.type == MODEM_JOB_SMS_TEMP_ALARM) {
                s_temp_alarm_sms_job_queued = false;
                if (sms_done) {
                    s_new_temp_alarm_pending = false;
                    if (no_target) {
                        ESP_LOGW(TAG, "temperature alarm SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "temperature alarm SMS sent to all numbers");
                    }
                } else {
                    ESP_LOGE(TAG, "temperature alarm SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_HUMIDITY_ALARM) {
                s_hum_alarm_sms_job_queued = false;
                if (sms_done) {
                    s_new_hum_alarm_pending = false;
                    if (no_target) {
                        ESP_LOGW(TAG, "humidity alarm SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "humidity alarm SMS sent to all numbers");
                    }
                } else {
                    ESP_LOGE(TAG, "humidity alarm SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_TEMP_RESTORED) {
                s_temp_restored_sms_job_queued = false;
                if (sms_done) {
                    s_restore_temp_pending = false;
                    if (!s_restore_hum_pending) {
                        s_alarm_restored_sms_pending = false;
                        s_alarm_restored_sms_sent = true;
                    }
                    if (no_target) {
                        ESP_LOGW(TAG, "temperature restored SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "temperature restored SMS sent to all numbers");
                    }
                } else {
                    ESP_LOGE(TAG, "temperature restored SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_HUM_RESTORED) {
                s_hum_restored_sms_job_queued = false;
                if (sms_done) {
                    s_restore_hum_pending = false;
                    if (!s_restore_temp_pending) {
                        s_alarm_restored_sms_pending = false;
                        s_alarm_restored_sms_sent = true;
                    }
                    if (no_target) {
                        ESP_LOGW(TAG, "humidity restored SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "humidity restored SMS sent to all numbers");
                    }
                } else {
                    ESP_LOGE(TAG, "humidity restored SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_POWER_LOST) {
                if (sms_done) {
                    s_power_lost_sms_pending = false;
                    s_power_lost_sms_sent = true;

                    s_power_lost_call_pending = true;
                    s_power_lost_call_done = false;
                    s_last_power_lost_call_try_ms = 0;

                    if (no_target) {
                        ESP_LOGW(TAG, "power lost SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "power lost SMS sent to all numbers -> queue power lost call");
                    }
                } else {
                    ESP_LOGE(TAG, "power lost SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_POWER_RESTORED) {
                if (sms_done) {
                    s_power_restored_sms_pending = false;
                    s_power_restored_sms_sent = true;

                    s_power_restored_call_pending = false;
                    s_power_restored_call_done = true;
                    s_last_power_restored_call_try_ms = 0;

                    if (no_target) {
                        ESP_LOGW(TAG, "power restored SMS skipped: no target");
                    } else {
                        ESP_LOGI(TAG, "power restored SMS sent to all numbers -> queue power restored call");
                    }
                } else {
                    ESP_LOGE(TAG, "power restored SMS partial/failed total=%d ok=%d fail=%d",
                             sms_res.total_enabled, sms_res.ok_count, sms_res.fail_count);
                }
            }
            else if (job.type == MODEM_JOB_SMS_NTC_FAULT) {
                s_ntc_fault_sms_job_queued = false;
                if (sms_done) {
                    s_ntc_fault_sms_pending = false;
                    s_ntc_fault_sms_sent = true;
                    s_ntc_fault_sms_sent_once = true;
                }
            }
            else if (job.type == MODEM_JOB_SMS_NTC_RESTORED) {
                s_ntc_restored_sms_job_queued = false;
                if (sms_done) {
                    s_ntc_restored_sms_pending = false;
                    s_ntc_restored_sms_sent = true;
                    s_ntc_fault_sms_sent_once = false;
                }
            }
            else if (job.type == MODEM_JOB_SMS_HUM_SENSOR_FAULT) {
                s_sht_fault_sms_job_queued = false;
                if (sms_done) {
                    s_sht_fault_sms_pending = false;
                    s_sht_fault_sms_sent = true;
                    s_sht_fault_sms_sent_once = true;
                }
            }
            else if (job.type == MODEM_JOB_SMS_HUM_SENSOR_RESTORED) {
                s_sht_restored_sms_job_queued = false;
                if (sms_done) {
                    s_sht_restored_sms_pending = false;
                    s_sht_restored_sms_sent = true;
                    s_sht_fault_sms_sent_once = false;
                }
            }

            if (!s_new_temp_alarm_pending && !s_new_hum_alarm_pending) {
                s_alarm_sms_sent = true;
                s_alarm_sms_pending = false;
            }

            break;
        }

        case MODEM_JOB_SMS_DAILY_HEALTH: {
            char sms[320];
            build_sms_for_job(job.type, sms, sizeof(sms));

            esp_err_t err = modem_service_send_sms(s_runtime_cfg.alarm_number1, sms);
            s_daily_health_sms_job_queued = false;

            if (err == ESP_OK) {
                s_daily_health_sms_sent = true;
                s_daily_health_sms_pending = false;
                s_daily_health_last_day = s_rtc_time.day;
                s_daily_health_last_month = s_rtc_time.month;
                s_daily_health_last_year = s_rtc_time.year;
                ESP_LOGI(TAG, "daily health SMS sent to number1");
            } else {
                ESP_LOGE(TAG, "daily health SMS failed: %s", esp_err_to_name(err));
            }
            break;
        }

        case MODEM_JOB_CALL_ALARM: {
            esp_err_t err = modem_service_make_call(s_runtime_cfg.alarm_number1,
                                                    APP_ALARM_CALL_DURATION_MS);

            s_alarm_call_job_queued = false;

            if (err == ESP_OK) {
                if (s_alarm_call_request_count > 0) {
                    s_alarm_call_request_count--;
                }

                if (s_alarm_call_request_count > 0) {
                    s_alarm_call_pending = true;
                    s_alarm_call_done = false;
                    s_last_call_try_ms = 0;
                    ESP_LOGW(TAG, "alarm call done, still pending next calls=%u",
                             (unsigned)s_alarm_call_request_count);
                } else {
                    s_alarm_call_done = true;
                    s_alarm_call_pending = false;
                    ESP_LOGI(TAG, "alarm call done, no more pending calls");
                }
            } else {
                s_alarm_call_pending = true;
                s_alarm_call_done = false;
                ESP_LOGE(TAG, "alarm call failed: %s", esp_err_to_name(err));
            }
            break;
        }

        case MODEM_JOB_CALL_ALARM_RESTORED: {
            esp_err_t err = modem_service_make_call(s_runtime_cfg.alarm_number1,
                                                    APP_ALARM_CALL_DURATION_MS);
            if (err == ESP_OK) {
                s_alarm_restored_call_done = true;
                s_alarm_restored_call_pending = false;
                s_restore_temp_pending = false;
                s_restore_hum_pending = false;
                ESP_LOGI(TAG, "alarm restored call done");
            } else {
                ESP_LOGE(TAG, "alarm restored call failed: %s", esp_err_to_name(err));
            }
            break;
        }

        case MODEM_JOB_CALL_POWER_LOST: {
            esp_err_t err = modem_service_make_call(s_runtime_cfg.alarm_number1,
                                                    APP_ALARM_CALL_DURATION_MS);
            if (err == ESP_OK) {
                s_power_lost_call_done = true;
                s_power_lost_call_pending = false;
                ESP_LOGI(TAG, "power lost call done");
            } else {
                ESP_LOGE(TAG, "power lost call failed: %s", esp_err_to_name(err));
            }
            break;
        }

        case MODEM_JOB_CALL_POWER_RESTORED: {
            esp_err_t err = modem_service_make_call(s_runtime_cfg.alarm_number1,
                                                    APP_ALARM_CALL_DURATION_MS);
            if (err == ESP_OK) {
                s_power_restored_call_done = true;
                s_power_restored_call_pending = false;
                ESP_LOGI(TAG, "power restored call done");
            } else {
                ESP_LOGE(TAG, "power restored call failed: %s", esp_err_to_name(err));
            }
            break;
        }

        case MODEM_JOB_POLL_INBOX: {
            char sender[32] = {0};
            char body[192] = {0};
            char reply[256] = {0};
            bool found = false;

            ESP_LOGW(TAG, "POLL_INBOX_START");

            esp_err_t err = modem_service_poll_unread_sms(sender, sizeof(sender),
                                                        body, sizeof(body), &found);

            ESP_LOGW(TAG,
                    "POLL_INBOX_RESULT: err=%s found=%d sender=[%s] body=[%s]",
                    esp_err_to_name(err),
                    found ? 1 : 0,
                    sender,
                    body);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "poll unread sms failed: %s", esp_err_to_name(err));
                break;
            }

            if (!found) {
                break;
            }

            ESP_LOGI(TAG, "incoming sms from=%s body=%s", sender, body);

            if (sms_command_text_equals_ci(body, "TT") ||
                sms_command_text_equals_ci(body, "INFOR")) {

                sms_command_format_status(reply, sizeof(reply));

                esp_err_t send_err = modem_service_send_sms(sender, reply);
                ESP_LOGW(TAG, "TT_REPLY_SEND: %s", esp_err_to_name(send_err));

            } else if (sms_command_text_equals_ci(body, "2511;ENG_INFOR")) {

                sms_command_format_eng_info(reply, sizeof(reply));

                esp_err_t send_err = modem_service_send_sms(sender, reply);
                ESP_LOGW(TAG, "ENG_REPLY_SEND: %s", esp_err_to_name(send_err));

            } else if (sms_command_text_equals_ci(body, "2511;RESET")) {

                esp_err_t send_err = modem_service_send_sms(sender, "RESET OK, REBOOT");
                ESP_LOGW(TAG, "RESET_REPLY_SEND: %s", esp_err_to_name(send_err));

                vTaskDelay(pdMS_TO_TICKS(1500));
                handle_reboot_request_internal();

            } else if (strncmp(body, "2511;", 5) == 0) {

                esp_err_t set_err = sms_command_process_set(sender, body);
                ESP_LOGW(TAG, "SET_SMS_PROCESS: %s", esp_err_to_name(set_err));

            } else {
                ESP_LOGI(TAG, "ignore sms command body=%s", body);
            }

            break;
        }
        default:
            ESP_LOGW(TAG, "unknown modem job=%d", (int)job.type);
            break;
        }

        s_modem_busy = false;
    }
}

static void process_incoming_sms_commands(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    if ((now_ms - s_last_sms_command_poll_ms) < APP_SMS_COMMAND_POLL_MS) {
        return;
    }

    if (s_modem_busy) {
        ESP_LOGW(TAG, "SMS_POLL_SKIP: modem_busy=1");
        return;
    }

    if (enqueue_modem_job(MODEM_JOB_POLL_INBOX)) {
        s_last_sms_command_poll_ms = now_ms;
        ESP_LOGW(TAG, "SMS_POLL_QUEUED");
    } else {
        ESP_LOGE(TAG, "SMS_POLL_QUEUE_FAILED");
    }
}

static void publish_task(void *arg)
{
    (void)arg;

    char payload[640];
    TickType_t last_health = xTaskGetTickCount();
    TickType_t last_sensor = 0;
    TickType_t last_display = 0;
    TickType_t last_blink = 0;
    TickType_t last_publish = 0;

    const TickType_t loop_tick = pdMS_TO_TICKS(100);

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_rtc_read) >= pdMS_TO_TICKS(APP_RTC_READ_MS)) {
            read_rtc_time();
            s_last_rtc_read = now;
        }
        if ((now - s_last_rtc_sync_check) >= pdMS_TO_TICKS(APP_RTC_SYNC_CHECK_MS)) {
            if (s_uplink_ready) {
                sync_system_time_to_ds1307_once();
            }
            s_last_rtc_sync_check = now;
        }
        if ((now - last_health) >= pdMS_TO_TICKS(APP_HEALTH_LOG_MS)) {
            diag_log_health("periodic", s_uplink_ready, mqtt_service_is_connected(), mqtt_service_get_outbox_size());
            publish_state_internal("periodic_health");
            last_health = now;
        }

        if ((now - last_sensor) >= pdMS_TO_TICKS(APP_SENSOR_SAMPLE_MS)) {
            sample_sensors();
            // if (s_ntc1_valid && (!s_runtime_cfg.hum_enabled || s_humidity_valid)) {
            //     s_boot_sensor_ready = true;
            // }
            if (s_ntc1_valid) {
                s_boot_sensor_ready = true;
            }
            if (!s_power_boot_checked) {
                power_sample_t sample = {0};
                power_state_t state = POWER_STATE_UNKNOWN;
                esp_err_t err = power_monitor_get_latest(&sample, &state);

                if (err == ESP_OK) {
                    s_power_boot_checked = true;
                    ESP_LOGW(TAG, "initial power state=%d main=%.2fV bk=%.2fV",
                             (int)state, sample.main_v, sample.bk_v);

                    if (state == POWER_STATE_MAIN_LOST) {
                        s_power_lost_sms_pending = true;
                        s_power_lost_sms_sent = false;
                        s_last_power_lost_sms_try_ms = 0;

                        s_power_lost_call_pending = false;
                        s_power_lost_call_done = false;
                        s_last_power_lost_call_try_ms = 0;

                        s_power_lost_last_notify_ms = esp_timer_get_time() / 1000;
                        s_power_lost_notify_count = 1;
                        update_do1_output_state();

                        ESP_LOGW(TAG, "boot with main power lost -> queue FIRST power lost SMS, call after SMS");
                    }
                }
            }

            process_alarm_logic();
            process_alarm_reminder();
            process_alarm_sms();
            process_alarm_restored_sms();

            process_power_lost_sms();
            process_power_restored_sms();
            
            process_daily_health_sms();

            process_ntc_fault_sms();
            process_ntc_restored_sms();

            process_sht_fault_sms();
            process_sht_restored_sms();
            process_power_lost_reminder();

            process_alarm_call();
            process_alarm_restored_call();
            process_power_lost_call();
            process_power_restored_call();

            process_incoming_sms_commands();
            process_sensor_fault_notifications();
            last_sensor = now;
        }

        if ((now - last_blink) >= pdMS_TO_TICKS(APP_BLINK_MS)) {
            s_blink_on = !s_blink_on;
            last_blink = now;
        }

        if ((now - last_display) >= pdMS_TO_TICKS(APP_DISPLAY_REFRESH_MS)) {
             update_display_brightness_by_power();
            update_display();
            last_display = now;
        }
        update_sim_led_from_modem_state();
        tm1638_wifi_led_tick_100ms();
        tm1638_server_led_tick_100ms();
        sim_led_tick_100ms();
        if ((now - last_publish) >= pdMS_TO_TICKS(s_pub_period_ms)) {
            if (s_uplink_ready && mqtt_service_is_connected()) {
                bool di1 = false;
                bool di2 = false;
                bool di3 = false;
                bool do1 = false;
                bool do2 = false;

                board_tca_read_pin(APP_DI1_TCA_PIN, &di1);
                board_tca_read_pin(APP_DI2_TCA_PIN, &di2);
                board_tca_read_pin(APP_DI3_TCA_PIN, &di3);

                board_tca_get_output_pin(APP_DO1_TCA_PIN, &do1);
                board_tca_get_output_pin(APP_DO2_TCA_PIN, &do2);

                power_sample_t power_sample = {0};
                power_state_t power_state = POWER_STATE_UNKNOWN;
                int main_mv = 0;
                int backup_mv = 0;

                if (power_monitor_get_latest(&power_sample, &power_state) == ESP_OK) {
                    main_mv = (int)(power_sample.main_v * 1000.0f);
                    backup_mv = (int)(power_sample.bk_v * 1000.0f);
                }

                float temp1_pub = s_last_temp1_c;
                float temp2_pub = s_last_temp2_c;
                float temp_sht_pub = s_last_sht30_temp_c;
            
                // float hum_sht_pub = (s_runtime_cfg.hum_enabled && s_humidity_valid)
                //     ? s_last_humidity : 0.0f;
                // HUM_OFF => gui 0; loi SHT30 => giu gia tri hop le cuoi cung
                float hum_sht_pub = s_runtime_cfg.hum_enabled ? s_last_humidity : 0.0f;
                ESP_LOGI(TAG, "telemetry io: di1=%d di2=%d di3=%d do1=%d do2=%d main=%d backup=%d",
                         di1 ? 1 : 0, di2 ? 1 : 0, di3 ? 1 : 0, do1 ? 1 : 0, do2 ? 1 : 0,
                         main_mv, backup_mv);

                snprintf(payload, sizeof(payload),
                         "{\"version\":\"%s\","
                         "\"temp1\":%.2f,"
                         "\"temp2\":%.2f,"
                         "\"temp_sht\":%.2f,"
                         "\"hum_sht\":%.2f,"
                         "\"dienap\":%d,"
                         "\"u_backup\":%d,"
                         "\"input1\":%s,"
                         "\"input2\":%s,"
                         "\"input3\":%s,"
                         "\"output1\":%s,"
                         "\"output2\":%s,"
                         "\"output3\":%s,"
                         "\"lat\":0,"
                         "\"lon\":0}",
                         APP_FW_VERSION,
                         temp1_pub,
                         temp2_pub,
                         temp_sht_pub,
                         hum_sht_pub,
                         main_mv,
                         backup_mv,
                         di1 ? "true" : "false",
                         di2 ? "true" : "false",
                         di3 ? "true" : "false",
                         do1 ? "true" : "false",
                         do2 ? "true" : "false",
                         "false");

                int mid = mqtt_service_publish_telemetry(payload);
                ESP_LOGI(TAG, "publish mid=%d payload=%s", mid, payload);
            } else {
                ESP_LOGW(TAG, "skip publish uplink=%d mqtt=%d net=%s temp1=%.2f valid1=%d temp2=%.2f valid2=%d hum=%.2f valid=%d",
                         s_uplink_ready ? 1 : 0,
                         mqtt_service_is_connected() ? 1 : 0,
                         net_type_to_str_internal(s_net_type),
                         s_last_temp1_c,
                         s_ntc1_valid ? 1 : 0,
                         s_last_temp2_c,
                         s_ntc2_valid ? 1 : 0,
                         s_last_humidity,
                         s_humidity_valid ? 1 : 0);
            }

            last_publish = now;
            s_publish_count++;
        }

        vTaskDelay(loop_tick);
    }
}


static void process_ntc_fault_sms(void)
{
    if (!s_ntc_fault_sms_pending || s_ntc_fault_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip NTC fault SMS");
        s_ntc_fault_sms_pending = false;
        s_ntc_fault_sms_sent = true;
        s_ntc_fault_sms_job_queued = false;
        s_ntc_fault_sms_sent_once = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_ntc_fault_sms_try_ms != 0 &&
        (now_ms - s_last_ntc_fault_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    if (!s_ntc_fault_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_NTC_FAULT)) {
            s_ntc_fault_sms_job_queued = true;
            s_last_ntc_fault_sms_try_ms = now_ms;
            ESP_LOGW(TAG, "queued NTC fault SMS");
        }
    }
}

static void process_ntc_restored_sms(void)
{
    if (!s_ntc_restored_sms_pending || s_ntc_restored_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip NTC restored SMS");
        s_ntc_restored_sms_pending = false;
        s_ntc_restored_sms_sent = true;
        s_ntc_restored_sms_job_queued = false;
        s_ntc_fault_sms_sent_once = false;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_ntc_restored_sms_try_ms != 0 &&
        (now_ms - s_last_ntc_restored_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    if (!s_ntc_restored_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_NTC_RESTORED)) {
            s_ntc_restored_sms_job_queued = true;
            s_last_ntc_restored_sms_try_ms = now_ms;
            ESP_LOGW(TAG, "queued NTC restored SMS");
        }
    }
}

static void process_sht_fault_sms(void)
{
    if (!s_sht_fault_sms_pending || s_sht_fault_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip humidity sensor fault SMS");
        s_sht_fault_sms_pending = false;
        s_sht_fault_sms_sent = true;
        s_sht_fault_sms_job_queued = false;
        s_sht_fault_sms_sent_once = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_sht_fault_sms_try_ms != 0 &&
        (now_ms - s_last_sht_fault_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    if (!s_sht_fault_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_HUM_SENSOR_FAULT)) {
            s_sht_fault_sms_job_queued = true;
            s_last_sht_fault_sms_try_ms = now_ms;
            ESP_LOGW(TAG, "queued humidity sensor fault SMS");
        }
    }
}

static void process_sht_restored_sms(void)
{
    if (!s_sht_restored_sms_pending || s_sht_restored_sms_sent) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip humidity sensor restored SMS");
        s_sht_restored_sms_pending = false;
        s_sht_restored_sms_sent = true;
        s_sht_restored_sms_job_queued = false;
        s_sht_fault_sms_sent_once = false;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_sht_restored_sms_try_ms != 0 &&
        (now_ms - s_last_sht_restored_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    if (!s_sht_restored_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_HUM_SENSOR_RESTORED)) {
            s_sht_restored_sms_job_queued = true;
            s_last_sht_restored_sms_try_ms = now_ms;
            ESP_LOGW(TAG, "queued humidity sensor restored SMS");
        }
    }
}

static void process_power_lost_call(void)
{
    if (!s_power_lost_call_pending || s_power_lost_call_done) {
        return;
    }

    if (!has_alarm_call_target()) {
        ESP_LOGW(TAG, "alarm_number1 empty/invalid -> skip power lost call");
        s_power_lost_call_pending = false;
        s_power_lost_call_done = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_power_lost_call_try_ms) < APP_ALARM_CALL_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_power_lost_call_try_ms = now_ms;

    if (enqueue_modem_job(MODEM_JOB_CALL_POWER_LOST)) {
        ESP_LOGW(TAG, "queued power lost call to number1");
    }
}

static void process_power_lost_sms(void)
{
    if (!s_power_lost_sms_pending || s_power_lost_sms_sent) {
        return;
    }

    if (!s_boot_sensor_ready) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip power lost SMS");
        s_power_lost_sms_pending = false;
        s_power_lost_sms_sent = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_power_lost_sms_try_ms != 0 &&
        (now_ms - s_last_power_lost_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_power_lost_sms_try_ms = now_ms;

    if (enqueue_modem_job(MODEM_JOB_SMS_POWER_LOST)) {
        ESP_LOGW(TAG, "queued power lost SMS");
    }
}

static void process_power_lost_reminder(void)
{
    power_sample_t sample = {0};
    power_state_t state = POWER_STATE_UNKNOWN;

    if (power_monitor_get_latest(&sample, &state) != ESP_OK) {
        return;
    }

    if (state != POWER_STATE_MAIN_LOST) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_power_lost_last_notify_ms == 0) {
        return;
    }

    if ((now_ms - s_power_lost_last_notify_ms) < APP_POWER_LOST_REMIND_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    /*
     * Lan 1: gui ngay khi mat dien
     * Lan 2: sau 10 phut van mat dien -> gui SMS + CALL
     * Tu lan 3 tro di: khong gui nua
     */
    if (s_power_lost_notify_count >= 3) {
        s_power_lost_last_notify_ms = now_ms;
        ESP_LOGW(TAG, "power still lost, no more SMS/call after second notification");
        return;
    }

    s_power_lost_sms_pending = true;
    s_power_lost_sms_sent = false;
    s_last_power_lost_sms_try_ms = 0;

    s_power_lost_call_pending = false;
    s_power_lost_call_done = false;
    s_last_power_lost_call_try_ms = 0;

    s_power_lost_last_notify_ms = now_ms;
    s_power_lost_notify_count++;

    ESP_LOGW(TAG, "power still lost after 10 minutes -> queue final reminder SMS+CALL, count=%d",
             s_power_lost_notify_count);
}

static void on_power_monitor_event(const power_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    ESP_LOGW(TAG, "power event: %s main=%.2fV bk=%.2fV seq=%lu",
             power_monitor_event_to_str(event->type),
             event->main_v,
             event->bk_v,
             (unsigned long)event->seq);

    if (event->type == POWER_EVENT_MAIN_LOST) {
        s_power_lost_sms_pending = true;
        s_power_lost_sms_sent = false;
        s_last_power_lost_sms_try_ms = 0;

        s_power_lost_call_pending = false;
        s_power_lost_call_done = false;
        s_last_power_lost_call_try_ms = 0;

        s_power_restored_sms_pending = false;
        s_power_restored_sms_sent = false;
        s_last_power_restored_sms_try_ms = 0;

        s_power_restored_call_pending = false;
        s_power_restored_call_done = false;
        s_last_power_restored_call_try_ms = 0;

        s_power_lost_last_notify_ms = esp_timer_get_time() / 1000;
        s_power_lost_notify_count = 1;

        update_do1_output_state();
    }
    else if (event->type == POWER_EVENT_MAIN_RESTORED) {
        s_power_restored_sms_pending = true;
        s_power_restored_sms_sent = false;
        s_last_power_restored_sms_try_ms = 0;

        s_power_restored_call_pending = false;
        s_power_restored_call_done = true;
        s_last_power_restored_call_try_ms = 0;

        s_power_lost_sms_pending = false;
        s_power_lost_sms_sent = false;
        s_last_power_lost_sms_try_ms = 0;

        s_power_lost_call_pending = false;
        s_power_lost_call_done = false;
        s_last_power_lost_call_try_ms = 0;

        s_power_lost_last_notify_ms = 0;
        s_power_lost_notify_count = 0;

        update_do1_output_state();

    }
}


static void process_power_restored_sms(void)
{
    if (!s_power_restored_sms_pending || s_power_restored_sms_sent) {
        return;
    }

    if (!s_boot_sensor_ready) {
        return;
    }

    if (!has_any_alarm_sms_target()) {
        ESP_LOGW(TAG, "no alarm phone configured -> skip power restored SMS");
        s_power_restored_sms_pending = false;
        s_power_restored_sms_sent = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_power_restored_sms_try_ms != 0 &&
        (now_ms - s_last_power_restored_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_power_restored_sms_try_ms = now_ms;

    if (enqueue_modem_job(MODEM_JOB_SMS_POWER_RESTORED)) {
        ESP_LOGW(TAG, "queued power restored SMS");
    }
}

static void process_power_restored_call(void)
{
    if (!s_power_restored_call_pending || s_power_restored_call_done) {
        return;
    }

    if (!has_alarm_call_target()) {
        ESP_LOGW(TAG, "alarm_number1 empty/invalid -> skip power restored call");
        s_power_restored_call_pending = false;
        s_power_restored_call_done = true;
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_power_restored_call_try_ms) < APP_ALARM_CALL_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    s_last_power_restored_call_try_ms = now_ms;

    if (enqueue_modem_job(MODEM_JOB_CALL_POWER_RESTORED)) {
        ESP_LOGW(TAG, "queued power restored call to number1");
    }
}

esp_err_t app_logic_init(void)
{
    s_ntc_fault_active = false;
    s_sht_fault_active = false;
    s_ntc_fault_notify_pending = false;
    s_sht_fault_notify_pending = false;
    snprintf(s_ntc_fault_reason, sizeof(s_ntc_fault_reason), "none");
    snprintf(s_sht_fault_reason, sizeof(s_sht_fault_reason), "none");
    s_ntc_fault_sms_pending = false;
    s_ntc_fault_sms_sent = false;
    s_last_ntc_fault_sms_try_ms = 0;
    s_ntc_fault_sms_job_queued = false;
    s_ntc_restored_sms_pending = false;
    s_ntc_restored_sms_sent = false;
    s_last_ntc_restored_sms_try_ms = 0;
    s_ntc_restored_sms_job_queued = false;
    s_ntc_fault_confirmed = false;
    s_ntc_fault_sms_sent_once = false;
    s_ntc_fault_confirm_count = 0;
    s_ntc_restore_confirm_count = 0;
    s_ntc_fault_confirmed = false;
    s_ntc_fault_sms_sent_once = false;
    s_ntc_fault_confirm_count = 0;
    s_ntc_restore_confirm_count = 0;

    s_sht_fault_sms_pending = false;
    s_sht_fault_sms_sent = false;
    s_last_sht_fault_sms_try_ms = 0;
    s_sht_fault_sms_job_queued = false;
    s_sht_restored_sms_pending = false;
    s_sht_restored_sms_sent = false;
    s_last_sht_restored_sms_try_ms = 0;
    s_sht_restored_sms_job_queued = false;

    s_sht_fault_confirmed = false;
    s_sht_fault_sms_sent_once = false;
    s_sht_fault_confirm_count = 0;
    s_sht_restore_confirm_count = 0;

    s_alarm_call_job_queued = false;
    s_alarm_call_request_count = 0;

    s_temp_alarm_last_notify_ms = 0;
    s_hum_alarm_last_notify_ms = 0;
    s_temp_alarm_notify_count = 0;
    s_hum_alarm_notify_count = 0;

    s_temp_restored_sms_job_queued = false;
    s_hum_restored_sms_job_queued = false;

    s_daily_health_sms_pending = false;
    s_daily_health_sms_sent = false;
    s_daily_health_sms_job_queued = false;
    s_daily_health_last_day = -1;
    s_daily_health_last_month = -1;
    s_daily_health_last_year = -1;
    s_last_daily_health_sms_try_ms = 0;
    app_logic_load_runtime_config_internal();
    s_pub_period_ms = s_runtime_cfg.telemetry_interval_ms;

    update_do1_output_state();
    
    s_net_type = APP_NET_NONE;
    s_uplink_ready = false;
    s_last_sht30_sample = 0;
    s_last_rtc_sync_check = 0;

    adc_oneshot_unit_handle_t pm_adc = power_monitor_get_adc_handle();
    if (pm_adc != NULL) {
        ntc_init(&s_ntc1, APP_NTC1_ADC_CHANNEL, pm_adc);
        s_ntc1_ready = true;
        s_last_temp1_c = 25.0f;
        s_ntc1_valid = true;

        ntc_init(&s_ntc2, APP_NTC2_ADC_CHANNEL, pm_adc);
        s_ntc2_ready = true;
        s_last_temp2_c = 25.0f;
        s_ntc2_valid = true;

        ESP_LOGI(TAG, "NTC1/NTC2 init OK with shared ADC");
    } else {
        s_ntc1_ready = false;
        s_last_temp1_c = 25.0f;
        s_ntc1_valid = false;

        s_ntc2_ready = false;
        s_last_temp2_c = 25.0f;
        s_ntc2_valid = false;

        ESP_LOGE(TAG, "NTC1/NTC2 init failed: shared ADC handle is NULL");
    }

    ESP_ERROR_CHECK(power_monitor_register_callback(on_power_monitor_event, NULL));
    
    memset(&s_rtc_time, 0, sizeof(s_rtc_time));
    s_rtc_valid = false;
    s_last_rtc_read = 0;
    s_rtc_synced_from_system = false;
    s_last_rtc_sync_day = -1;

    esp_err_t rtc_err = ds1307_init(APP_I2C_MASTER_PORT);
    ESP_LOGI(TAG, "ds1307 init: %s", esp_err_to_name(rtc_err));

    if (rtc_err == ESP_OK) {
        sync_system_time_from_ds1307_once();
    }
    if (sht30_init(&s_sht30,
                   APP_SHT30_I2C_PORT,
                   APP_SHT30_SCL_GPIO,
                   APP_SHT30_SDA_GPIO,
                   APP_SHT30_I2C_ADDR,
                   APP_SHT30_I2C_FREQ_HZ,
                   0) == ok) {
        s_sht30_ready = true;
        ESP_LOGI(TAG, "SHT30 ready on I2C%d SCL=%d SDA=%d",
                 APP_SHT30_I2C_PORT,
                 APP_SHT30_SCL_GPIO,
                 APP_SHT30_SDA_GPIO);
    } else {
        s_sht30_ready = false;
        ESP_LOGW(TAG, "SHT30 init failed, humidity telemetry disabled");
    }

    if (tm1638_init(&s_tm1638,
                    APP_TM1638_STB_GPIO,
                    APP_TM1638_CLK_GPIO,
                    APP_TM1638_DIO_GPIO,
                    APP_TM1638_BRIGHTNESS) == ESP_OK) {
        s_display_ready = true;
        tm1638_clear(&s_tm1638);
        tm1638_wifi_bind(&s_tm1638);
        tm1638_server_bind(&s_tm1638);
        tm1638_server_set_state(SERVER_LED_ERROR_BLINK);

        sim_led_bind(&s_tm1638);
        sim_led_set_index(6);                  // LED SIM index 6
        //  // luc moi khoi dong: tat
        sim_led_set_state(SIM_LED_STATE_SEARCHING);

        tm1638_server_set_state(SERVER_LED_ERROR_BLINK);
        
        s_last_display_brightness = 0xFF;
        update_display_brightness_by_power();

        ESP_LOGI(TAG, "TM1638 ready");
    } else {
        s_display_ready = false;
        ESP_LOGW(TAG, "TM1638 init failed, local display disabled");
    }

    s_last_sht30_temp_c = 0.0f;
    s_last_humidity = 0.0f;
    s_humidity_valid = false;
    s_blink_on = true;

    s_alarm_active = false;
    s_alarm_sms_pending = false;
    s_alarm_sms_sent = false;
    s_last_sms_try_ms = 0;
    s_alarm_call_pending = false;
    s_alarm_call_done = false;
    s_last_call_try_ms = 0;
    //s_alarm_notify_count = 0;

    s_temp_alarm_last_notify_ms = 0;
    s_hum_alarm_last_notify_ms = 0;
    s_temp_alarm_notify_count = 0;
    s_hum_alarm_notify_count = 0;

    s_alarm_restored_sms_pending = false;
    s_alarm_restored_sms_sent = false;
    s_last_alarm_restored_sms_try_ms = 0;
    s_temp_alarm_sms_job_queued = false;
    s_hum_alarm_sms_job_queued = false;
    s_alarm_restored_sms_job_queued = false;
    s_alarm_restored_call_pending = false;
    s_alarm_restored_call_done = false;
    s_last_alarm_restored_call_try_ms = 0;
    s_alarm_temp_was_active = false;
    s_alarm_hum_was_active = false;
    s_temp_low_alarm_active = false;
    s_temp_high_alarm_active = false;
    s_hum_low_alarm_active = false;
    s_hum_high_alarm_active = false;
    s_new_temp_alarm_pending = false;
    s_new_hum_alarm_pending = false;
    s_restore_temp_pending = false;
    s_restore_hum_pending = false;
    s_alarm_assert_start_ms = 0;
    s_alarm_restore_start_ms = 0;
   // s_alarm_last_notify_ms = 0;

    s_power_lost_sms_pending = false;
    s_power_lost_sms_sent = false;
    s_last_power_lost_sms_try_ms = 0;

    s_power_restored_sms_pending = false;
    s_power_restored_sms_sent = false;
    s_last_power_restored_sms_try_ms = 0;

    s_power_lost_call_pending = false;
    s_power_lost_call_done = false;
    s_last_power_lost_call_try_ms = 0;

    s_power_restored_call_pending = false;
    s_power_restored_call_done = false;
    s_last_power_restored_call_try_ms = 0;

    s_power_lost_last_notify_ms = 0;
    s_power_lost_notify_count = 0;

    s_power_boot_checked = false;
    s_last_sms_command_poll_ms = 0;
    s_modem_busy = false;
    s_ntc_fault_sms_pending = false;
    s_ntc_fault_sms_sent = false;
    s_last_ntc_fault_sms_try_ms = 0;
    s_ntc_fault_sms_job_queued = false;
    s_ntc_restored_sms_pending = false;
    s_ntc_restored_sms_sent = false;
    s_last_ntc_restored_sms_try_ms = 0;
    s_ntc_restored_sms_job_queued = false;
    s_sht_fault_sms_pending = false;
    s_sht_fault_sms_sent = false;
    s_last_sht_fault_sms_try_ms = 0;
    s_sht_fault_sms_job_queued = false;
    s_sht_restored_sms_pending = false;
    s_sht_restored_sms_sent = false;
    s_last_sht_restored_sms_try_ms = 0;
    s_sht_restored_sms_job_queued = false;

    esp_err_t del_err = modem_service_delete_all_sms();
    ESP_LOGI(TAG, "delete all sms on boot: %s", esp_err_to_name(del_err));

    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_CLOUD_COMMAND, app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_MQTT_CONNECTED, app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_NET_UP, app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_NET_DOWN, app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_MQTT_DISCONNECTED, app_event_handler, NULL));

    time_sync_start();
    ESP_LOGW(TAG, "forced time_sync_start() from app_logic_init");

    if (s_modem_queue == NULL) {
        s_modem_queue = xQueueCreate(16, sizeof(modem_job_t));
        if (s_modem_queue == NULL) {
            return ESP_FAIL;
        }
    }

    if (s_publish_task == NULL) {
        BaseType_t ok_pub = xTaskCreate(publish_task, "publish_task", 12288, NULL, 8, &s_publish_task);
        if (ok_pub != pdPASS) {
            return ESP_FAIL;
        }
    }

    if (s_modem_task == NULL) {
        BaseType_t ok_modem = xTaskCreate(modem_task, "modem_task", 8192, NULL, 7, &s_modem_task);
        if (ok_modem != pdPASS) {
            return ESP_FAIL;
        }
    }
    if (s_cloud_cmd_queue == NULL) {
        s_cloud_cmd_queue = xQueueCreate(8, sizeof(app_cloud_cmd_t));
        if (s_cloud_cmd_queue == NULL) {
            return ESP_FAIL;
        }
    }

    if (s_cloud_cmd_task == NULL) {
        BaseType_t ok_cmd = xTaskCreate(cloud_cmd_task,
                                        "cloud_cmd_task",
                                        16384,
                                        NULL,
                                        8,
                                        &s_cloud_cmd_task);
        if (ok_cmd != pdPASS) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* ===== app_logic public bridge for future module split ===== */
void app_logic_get_context(app_logic_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->runtime_cfg = &s_runtime_cfg;

    ctx->pub_period_ms = &s_pub_period_ms;
    ctx->uplink_ready = &s_uplink_ready;
    ctx->net_type = &s_net_type;
    ctx->modem_busy = &s_modem_busy;

    ctx->last_temp1_c = (float *)&s_last_temp1_c;
    ctx->last_temp2_c = (float *)&s_last_temp2_c;
    ctx->last_sht30_temp_c = (float *)&s_last_sht30_temp_c;
    ctx->last_humidity = (float *)&s_last_humidity;

    ctx->ntc1_valid = (bool *)&s_ntc1_valid;
    ctx->ntc2_valid = (bool *)&s_ntc2_valid;
    ctx->humidity_valid = (bool *)&s_humidity_valid;

    ctx->alarm_active = &s_alarm_active;
    ctx->alarm_sms_sent = &s_alarm_sms_sent;
    ctx->alarm_call_done = &s_alarm_call_done;

    ctx->alarm_temp_was_active = &s_alarm_temp_was_active;
    ctx->alarm_hum_was_active = &s_alarm_hum_was_active;

    ctx->temp_low_alarm_active = &s_temp_low_alarm_active;
    ctx->temp_high_alarm_active = &s_temp_high_alarm_active;
    ctx->hum_low_alarm_active = &s_hum_low_alarm_active;
    ctx->hum_high_alarm_active = &s_hum_high_alarm_active;

    ctx->new_temp_alarm_pending = &s_new_temp_alarm_pending;
    ctx->new_hum_alarm_pending = &s_new_hum_alarm_pending;
    ctx->restore_temp_pending = &s_restore_temp_pending;
    ctx->restore_hum_pending = &s_restore_hum_pending;

    ctx->temp_limit_sms_recent = &s_temp_limit_sms_recent;
    ctx->temp_limit_sms_recent_ms = &s_temp_limit_sms_recent_ms;
    ctx->hum_limit_sms_recent = &s_hum_limit_sms_recent;
    ctx->hum_limit_sms_recent_ms = &s_hum_limit_sms_recent_ms;

    ctx->last_set_config_ms = &s_last_set_config_ms;
    ctx->last_set_config_payload = s_last_set_config_payload;
    ctx->last_set_config_payload_len = sizeof(s_last_set_config_payload);

    ctx->pending_mqtt_temp_limit_valid = &s_pending_mqtt_temp_limit_valid;
    ctx->pending_mqtt_temp_low = &s_pending_mqtt_temp_low;
    ctx->pending_mqtt_temp_high = &s_pending_mqtt_temp_high;
    ctx->pending_mqtt_temp_confirm_count = &s_pending_mqtt_temp_confirm_count;
    ctx->pending_mqtt_temp_first_ms = &s_pending_mqtt_temp_first_ms;

    ctx->pending_mqtt_hum_limit_valid = &s_pending_mqtt_hum_limit_valid;
    ctx->pending_mqtt_hum_low = &s_pending_mqtt_hum_low;
    ctx->pending_mqtt_hum_high = &s_pending_mqtt_hum_high;
    ctx->pending_mqtt_hum_confirm_count = &s_pending_mqtt_hum_confirm_count;
    ctx->pending_mqtt_hum_first_ms = &s_pending_mqtt_hum_first_ms;
}

void app_logic_load_runtime_config(void)
{
    app_logic_load_runtime_config_internal();
}

void app_logic_publish_state(const char *reason)
{
    publish_state_internal(reason);
}

void app_logic_reply_current_state(const char *request_id, bool ok, const char *extra)
{
    reply_current_state_internal(request_id, ok, extra);
}

void app_logic_handle_reboot_request(void)
{
    handle_reboot_request_internal();
}

void app_logic_handle_factory_reset_request(void)
{
    handle_factory_reset_request_internal();
}

void app_logic_trigger_alarm_immediately_after_limit_update(bool temp_limit_changed, bool hum_limit_changed)
{
    trigger_alarm_immediately_after_limit_update_internal(temp_limit_changed, hum_limit_changed);
}

bool app_logic_is_temp_low_trigger_now(void)
{
    return is_temp_low_trigger_now_internal();
}

bool app_logic_is_temp_high_trigger_now(void)
{
    return is_temp_high_trigger_now_internal();
}

bool app_logic_is_hum_low_trigger_now(void)
{
    return is_hum_low_trigger_now_internal();
}

bool app_logic_is_hum_high_trigger_now(void)
{
    return is_hum_high_trigger_now_internal();
}

const char *app_logic_get_power_status_text(void)
{
    return get_power_status_text_internal();
}

const char *app_logic_net_type_to_str(app_net_type_t type)
{
    return net_type_to_str_internal(type);
}

void app_logic_reset_pending_mqtt_temp_limit(void)
{
    reset_pending_mqtt_temp_limit_internal();
}

void app_logic_reset_pending_mqtt_hum_limit(void)
{
    reset_pending_mqtt_hum_limit_internal();
}

bool app_logic_mqtt_temp_limit_should_wait_for_confirm(float low, float high, int64_t now_ms)
{
    return mqtt_temp_limit_should_wait_for_confirm_internal(low, high, now_ms);
}

bool app_logic_mqtt_hum_limit_should_wait_for_confirm(float low, float high, int64_t now_ms)
{
    return mqtt_hum_limit_should_wait_for_confirm_internal(low, high, now_ms);
}

bool app_logic_register_pending_mqtt_temp_limit(float low, float high, int64_t now_ms)
{
    return register_pending_mqtt_temp_limit_internal(low, high, now_ms);
}

bool app_logic_register_pending_mqtt_hum_limit(float low, float high, int64_t now_ms)
{
    return register_pending_mqtt_hum_limit_internal(low, high, now_ms);
}

static void read_rtc_time(void)
{
    ds1307_time_t rtc;
    esp_err_t err = ds1307_read_time(&rtc);

    if (err == ESP_OK) {
        s_rtc_time = rtc;
        s_rtc_valid = true;

        ESP_LOGI(TAG, "RTC %02u:%02u:%02u %02u/%02u/%04u",
                 s_rtc_time.hour,
                 s_rtc_time.minute,
                 s_rtc_time.second,
                 s_rtc_time.day,
                 s_rtc_time.month,
                 s_rtc_time.year);
    } else {
        s_rtc_valid = false;
        ESP_LOGW(TAG, "RTC read failed: %s", esp_err_to_name(err));
    }
}

static bool is_daily_health_target_enabled(void)
{
    return alarm_number_enabled(s_runtime_cfg.alarm_number1);
}

static void build_daily_health_sms(char *out, size_t out_len)
{
    char time_buf[48];
    char power_buf[16];

    if (s_rtc_valid) {
        snprintf(time_buf, sizeof(time_buf),
                 "%02uh%02u - %02u/%02u/%04u",
                 s_rtc_time.hour,
                 s_rtc_time.minute,
                 s_rtc_time.day,
                 s_rtc_time.month,
                 s_rtc_time.year);
    } else {
        sms_command_format_time(time_buf, sizeof(time_buf));
    }

    snprintf(power_buf, sizeof(power_buf), "%s", get_power_status_text_internal());

    float temp_now = s_ntc1_valid ? s_last_temp1_c : 0.0f;
    float hum_now  = (s_runtime_cfg.hum_enabled && s_humidity_valid) ? s_last_humidity : 0.0f;

    snprintf(out, out_len,
             "Thong Bao - Thiet Bi: %s (%s); Dang Hoat Dong; %.2f (%.0f-%.0f); %.0f%% (%.0f-%.0f); %s; %s.",
             mqtt_service_get_device_key(),
             APP_FW_VERSION,
             temp_now,
             s_runtime_cfg.ntc_low_limit_c,
             s_runtime_cfg.ntc_high_limit_c,
             hum_now,
             s_runtime_cfg.hum_low_limit_pct,
             s_runtime_cfg.hum_high_limit_pct,
             power_buf,
             time_buf);
}

static void process_daily_health_sms(void)
{
    if (!s_rtc_valid) {
        return;
    }

    if (!is_daily_health_target_enabled()) {
        return;
    }

    /* Khong chen vao luc dang co alarm/power/fault quan trong */
    if (s_alarm_sms_pending ||
        s_alarm_call_pending ||
        s_alarm_restored_sms_pending ||
        s_power_lost_sms_pending ||
        s_power_lost_call_pending ||
        s_power_restored_sms_pending ||
        s_power_restored_call_pending ||
        s_ntc_fault_sms_pending ||
        s_ntc_restored_sms_pending ||
        s_sht_fault_sms_pending ||
        s_sht_restored_sms_pending) {
        return;
    }

    /* Moi ngay chi gui 1 lan */
    bool already_sent_today =
        (s_daily_health_last_day == s_rtc_time.day) &&
        (s_daily_health_last_month == s_rtc_time.month) &&
        (s_daily_health_last_year == (int)s_rtc_time.year);

    if (already_sent_today) {
        return;
    }

    /* Den 07:00 thi kich hoat */
    if (s_rtc_time.hour == 7 && s_rtc_time.minute < 5) {
        s_daily_health_sms_pending = true;
        s_daily_health_sms_sent = false;
    }

    if (!s_daily_health_sms_pending || s_daily_health_sms_sent) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_last_daily_health_sms_try_ms != 0 &&
        (now_ms - s_last_daily_health_sms_try_ms) < APP_SMS_RETRY_MS) {
        return;
    }

    if (s_modem_busy) {
        return;
    }

    if (!s_daily_health_sms_job_queued) {
        if (enqueue_modem_job(MODEM_JOB_SMS_DAILY_HEALTH)) {
            s_daily_health_sms_job_queued = true;
            s_last_daily_health_sms_try_ms = now_ms;
            ESP_LOGW(TAG, "queued daily health SMS");
        }
    }
}

static void sync_system_time_to_ds1307_once(void)
{
    time_t now = time(NULL);

    struct tm tm_now;
    localtime_r(&now, &tm_now);

    ESP_LOGW(TAG, "RTC sync check: uplink=%d now=%lld sys=%02d:%02d:%02d %02d/%02d/%04d synced=%d last_day=%d",
             s_uplink_ready ? 1 : 0,
             (long long)now,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec,
             tm_now.tm_mday,
             tm_now.tm_mon + 1,
             tm_now.tm_year + 1900,
             s_rtc_synced_from_system ? 1 : 0,
             s_last_rtc_sync_day);

    if ((tm_now.tm_year + 1900) < 2025) {
        ESP_LOGW(TAG, "RTC sync skipped: system time not valid");
        return;
    }

    int today = (tm_now.tm_year + 1900) * 10000 +
                (tm_now.tm_mon + 1) * 100 +
                tm_now.tm_mday;

    if (s_rtc_synced_from_system && s_last_rtc_sync_day == today) {
        ESP_LOGW(TAG, "RTC sync skipped: already synced today");
        return;
    }

    ds1307_time_t rtc_set = {
        .second = (uint8_t)tm_now.tm_sec,
        .minute = (uint8_t)tm_now.tm_min,
        .hour = (uint8_t)tm_now.tm_hour,
        .day_of_week = (uint8_t)(tm_now.tm_wday == 0 ? 7 : tm_now.tm_wday),
        .day = (uint8_t)tm_now.tm_mday,
        .month = (uint8_t)(tm_now.tm_mon + 1),
        .year = (uint16_t)(tm_now.tm_year + 1900),
    };

    esp_err_t err = ds1307_set_time(&rtc_set);
    if (err == ESP_OK) {
        s_rtc_synced_from_system = true;
        s_last_rtc_sync_day = today;

        ESP_LOGW(TAG, "RTC sync OK -> DS1307 = %02u:%02u:%02u %02u/%02u/%04u",
                 rtc_set.hour, rtc_set.minute, rtc_set.second,
                 rtc_set.day, rtc_set.month, rtc_set.year);
    } else {
        ESP_LOGW(TAG, "RTC sync failed: %s", esp_err_to_name(err));
    }
}

void app_logic_handle_web_limit_update(float old_ntc_low,
                                       float old_ntc_high,
                                       float old_hum_low_limit,
                                       float old_hum_high_limit)
{
    bool old_temp_low_alarm  = s_ntc1_valid && (s_last_temp1_c < old_ntc_low);
    bool old_temp_high_alarm = s_ntc1_valid && (s_last_temp1_c > old_ntc_high);
    bool old_hum_low_alarm   = s_runtime_cfg.hum_enabled && s_humidity_valid &&
                               (s_last_humidity < old_hum_low_limit);
    bool old_hum_high_alarm  = s_runtime_cfg.hum_enabled && s_humidity_valid &&
                               (s_last_humidity > old_hum_high_limit);

    bool new_temp_low_alarm  = s_ntc1_valid && (s_last_temp1_c < s_runtime_cfg.ntc_low_limit_c);
    bool new_temp_high_alarm = s_ntc1_valid && (s_last_temp1_c > s_runtime_cfg.ntc_high_limit_c);
    bool new_hum_low_alarm   = s_runtime_cfg.hum_enabled && s_humidity_valid &&
                               (s_last_humidity < s_runtime_cfg.hum_low_limit_pct);
    bool new_hum_high_alarm  = s_runtime_cfg.hum_enabled && s_humidity_valid &&
                               (s_last_humidity > s_runtime_cfg.hum_high_limit_pct);

    bool had_temp_alarm = old_temp_low_alarm || old_temp_high_alarm;
    bool has_temp_alarm = new_temp_low_alarm || new_temp_high_alarm;
    bool had_hum_alarm  = old_hum_low_alarm || old_hum_high_alarm;
    bool has_hum_alarm  = new_hum_low_alarm || new_hum_high_alarm;

    bool temp_alarm_kind_changed =
        (old_temp_low_alarm != new_temp_low_alarm) ||
        (old_temp_high_alarm != new_temp_high_alarm);

    bool hum_alarm_kind_changed =
        (old_hum_low_alarm != new_hum_low_alarm) ||
        (old_hum_high_alarm != new_hum_high_alarm);

    if ((!had_temp_alarm && has_temp_alarm) ||
        (had_temp_alarm && has_temp_alarm && temp_alarm_kind_changed)) {
        s_alarm_temp_was_active = false;
        s_temp_low_alarm_active = false;
        s_temp_high_alarm_active = false;

        trigger_alarm_immediately_after_limit_update_internal(true, false);
    }

    if ((!had_hum_alarm && has_hum_alarm) ||
        (had_hum_alarm && has_hum_alarm && hum_alarm_kind_changed)) {
        s_alarm_hum_was_active = false;
        s_hum_low_alarm_active = false;
        s_hum_high_alarm_active = false;

        trigger_alarm_immediately_after_limit_update_internal(false, true);
    }
}

void app_logic_handle_hum_disabled(void)
{
    s_alarm_hum_was_active = false;
    s_hum_low_alarm_active = false;
    s_hum_high_alarm_active = false;

    s_new_hum_alarm_pending = false;
    s_restore_hum_pending = false;
    s_hum_alarm_sms_job_queued = false;

    s_hum_alarm_last_notify_ms = 0;
    s_hum_alarm_notify_count = 0;

    if (!s_alarm_temp_was_active) {
        s_alarm_active = false;
        s_alarm_sms_pending = false;
        s_alarm_sms_sent = false;
        s_last_sms_try_ms = 0;

        s_alarm_call_pending = false;
        s_alarm_call_done = false;
        s_last_call_try_ms = 0;

        s_alarm_restored_sms_pending = false;
        s_alarm_restored_sms_sent = false;
        s_last_alarm_restored_sms_try_ms = 0;

        s_alarm_restored_call_pending = false;
        s_alarm_restored_call_done = true;
        s_last_alarm_restored_call_try_ms = 0;

        update_do1_output_state();
    }

    ESP_LOGW(TAG, "humidity disabled -> cleared humidity alarm state");
}

esp_err_t app_logic_get_log_snapshot(app_logic_log_snapshot_t *out)
{
    // if (!out) {
    //     return ESP_ERR_INVALID_ARG;
    // }

    // memset(out, 0, sizeof(*out));

    // snprintf(out->device_id, sizeof(out->device_id), "%s", APP_DEVICE_ID);
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));


    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    if (mac_err == ESP_OK) {
        snprintf(out->device_id,
                 sizeof(out->device_id),
                 "%s%02X%02X%02X%02X%02X%02X",
                 APP_DEVICE_ID,
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    } else {
        snprintf(out->device_id,
                 sizeof(out->device_id),
                 "%s",
                 APP_DEVICE_ID);
    }

    out->temp1 = s_last_temp1_c;
    out->temp2 = s_last_temp2_c;
    out->sht30_temp = s_last_sht30_temp_c;
    out->humidity = s_last_humidity;

    out->temp1_valid = s_ntc1_valid;
    out->temp2_valid = s_ntc2_valid;
    out->humidity_valid = s_humidity_valid;

    out->hum_enabled = s_runtime_cfg.hum_enabled;

    out->alarm_active = s_alarm_active;

    power_sample_t power_sample = {0};
    power_state_t power_state = POWER_STATE_UNKNOWN;

    if (power_monitor_get_latest(&power_sample, &power_state) == ESP_OK) {
        out->main_v = power_sample.main_v;
        out->backup_v = power_sample.bk_v;
        out->power_ok = (power_state == POWER_STATE_MAIN_OK);
    } else {
        out->main_v = 0.0f;
        out->backup_v = 0.0f;
        out->power_ok = false;
    }

    board_tca_read_pin(APP_DI1_TCA_PIN, &out->di1);
    board_tca_read_pin(APP_DI2_TCA_PIN, &out->di2);
    board_tca_read_pin(APP_DI3_TCA_PIN, &out->di3);

    board_tca_get_output_pin(APP_DO1_TCA_PIN, &out->out1);
    board_tca_get_output_pin(APP_DO2_TCA_PIN, &out->out2);

    return ESP_OK;
}

static bool system_time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};

    localtime_r(&now, &tm_now);

    return (tm_now.tm_year + 1900) >= 2025;
}

static esp_err_t sync_system_time_from_ds1307_once(void)
{
    if (system_time_is_valid()) {
        ESP_LOGI(TAG, "System time already valid, skip DS1307 -> system sync");
        return ESP_OK;
    }

    ds1307_time_t rtc = {0};

    esp_err_t err = ds1307_get_time(&rtc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read DS1307 failed: %s", esp_err_to_name(err));
        return err;
    }

    if (rtc.year < 2025 || rtc.year > 2099 ||
        rtc.month < 1 || rtc.month > 12 ||
        rtc.day < 1 || rtc.day > 31 ||
        rtc.hour > 23 ||
        rtc.minute > 59 ||
        rtc.second > 59) {
        ESP_LOGW(TAG,
                 "DS1307 time invalid: %02u:%02u:%02u %02u/%02u/%04u",
                 rtc.hour,
                 rtc.minute,
                 rtc.second,
                 rtc.day,
                 rtc.month,
                 rtc.year);
        return ESP_ERR_INVALID_RESPONSE;
    }

    setenv("TZ", "ICT-7", 1);
    tzset();

    struct tm tm_rtc = {0};

    tm_rtc.tm_sec  = rtc.second;
    tm_rtc.tm_min  = rtc.minute;
    tm_rtc.tm_hour = rtc.hour;
    tm_rtc.tm_mday = rtc.day;
    tm_rtc.tm_mon  = rtc.month - 1;
    tm_rtc.tm_year = rtc.year - 1900;
    tm_rtc.tm_isdst = -1;

    time_t t = mktime(&tm_rtc);
    if (t <= 0) {
        ESP_LOGW(TAG, "mktime from DS1307 failed");
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday from DS1307 failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "System time synced from DS1307: %02u:%02u:%02u %02u/%02u/%04u",
             rtc.hour,
             rtc.minute,
             rtc.second,
             rtc.day,
             rtc.month,
             rtc.year);

    return ESP_OK;
}

static void update_display_brightness_by_power(void)
{
    if (!s_display_ready) {
        return;
    }

    power_sample_t sample = {0};
    power_state_t power_state = POWER_STATE_UNKNOWN;
    uint8_t target = APP_TM1638_BRIGHTNESS_MAIN_OK;

    if (power_monitor_get_latest(&sample, &power_state) == ESP_OK) {
        if (power_state == POWER_STATE_MAIN_LOST) {
            target = APP_TM1638_BRIGHTNESS_MAIN_LOST;
        }
    }

    if (target == s_last_display_brightness) {
        return;
    }

    esp_err_t err = tm1638_set_brightness(&s_tm1638, target);
    if (err == ESP_OK) {
        s_last_display_brightness = target;
        ESP_LOGI(TAG, "display brightness set to %u", target);
    } else {
        ESP_LOGW(TAG, "display brightness update failed: %s", esp_err_to_name(err));
    }
}

static void update_sim_led_from_modem_state(void)
{
    if (modem_service_is_sim_ready()) {
        sim_led_set_state(SIM_LED_STATE_READY);
        return;
    }

    modem_state_t ms = modem_service_get_state();

    switch (ms) {
    case MODEM_STATE_INIT:
    case MODEM_STATE_SYNC:
    case MODEM_STATE_DATA:
    case MODEM_STATE_WAIT_IP:
    case MODEM_STATE_RECOVERING:
        sim_led_set_state(SIM_LED_STATE_SEARCHING);
        break;

    case MODEM_STATE_OFF:
    default:
        sim_led_set_state(SIM_LED_STATE_OFF);
        break;
    }
}