#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_events.h"
#include "runtime_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    runtime_config_t *runtime_cfg;

    volatile int *pub_period_ms;
    volatile bool *uplink_ready;
    volatile app_net_type_t *net_type;
    volatile bool *modem_busy;

    float *last_temp1_c;
    float *last_temp2_c;
    float *last_sht30_temp_c;
    float *last_humidity;

    bool *ntc1_valid;
    bool *ntc2_valid;
    bool *humidity_valid;

    bool *alarm_active;
    bool *alarm_sms_sent;
    bool *alarm_call_done;

    bool *alarm_temp_was_active;
    bool *alarm_hum_was_active;

    bool *temp_low_alarm_active;
    bool *temp_high_alarm_active;
    bool *hum_low_alarm_active;
    bool *hum_high_alarm_active;

    bool *new_temp_alarm_pending;
    bool *new_hum_alarm_pending;
    bool *restore_temp_pending;
    bool *restore_hum_pending;

    bool *temp_limit_sms_recent;
    int64_t *temp_limit_sms_recent_ms;
    bool *hum_limit_sms_recent;
    int64_t *hum_limit_sms_recent_ms;

    int64_t *last_set_config_ms;
    char *last_set_config_payload;
    size_t last_set_config_payload_len;

    bool *pending_mqtt_temp_limit_valid;
    float *pending_mqtt_temp_low;
    float *pending_mqtt_temp_high;
    int *pending_mqtt_temp_confirm_count;
    int64_t *pending_mqtt_temp_first_ms;

    bool *pending_mqtt_hum_limit_valid;
    float *pending_mqtt_hum_low;
    float *pending_mqtt_hum_high;
    int *pending_mqtt_hum_confirm_count;
    int64_t *pending_mqtt_hum_first_ms;
} app_logic_ctx_t;

void app_logic_get_context(app_logic_ctx_t *ctx);

/* app_logic cung cap cac helper nay */
void app_logic_load_runtime_config(void);
void app_logic_publish_state(const char *reason);
void app_logic_reply_current_state(const char *request_id, bool ok, const char *extra);
void app_logic_handle_reboot_request(void);
void app_logic_handle_factory_reset_request(void);
void app_logic_trigger_alarm_immediately_after_limit_update(bool temp_limit_changed, bool hum_limit_changed);

bool app_logic_is_temp_low_trigger_now(void);
bool app_logic_is_temp_high_trigger_now(void);
bool app_logic_is_hum_low_trigger_now(void);
bool app_logic_is_hum_high_trigger_now(void);

const char *app_logic_get_power_status_text(void);
const char *app_logic_net_type_to_str(app_net_type_t type);

void app_logic_reset_pending_mqtt_temp_limit(void);
void app_logic_reset_pending_mqtt_hum_limit(void);
bool app_logic_mqtt_temp_limit_should_wait_for_confirm(float low, float high, int64_t now_ms);
bool app_logic_mqtt_hum_limit_should_wait_for_confirm(float low, float high, int64_t now_ms);
bool app_logic_register_pending_mqtt_temp_limit(float low, float high, int64_t now_ms);
bool app_logic_register_pending_mqtt_hum_limit(float low, float high, int64_t now_ms);

#ifdef __cplusplus
}
#endif