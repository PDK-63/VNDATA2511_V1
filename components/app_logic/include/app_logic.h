#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define TCA_PIN(port, bit)   ((port) * 8 + (bit))

#define APP_DO1_TCA_PIN      TCA_PIN(0, 1)
#define APP_DO2_TCA_PIN      TCA_PIN(0, 0)

#define APP_DI1_TCA_PIN      TCA_PIN(1, 7)
#define APP_DI2_TCA_PIN      TCA_PIN(1, 6)
#define APP_DI3_TCA_PIN      TCA_PIN(1, 5)

typedef struct {
    char device_id[32];

    float temp1;
    float temp2;
    float sht30_temp;
    float humidity;

    bool temp1_valid;
    bool temp2_valid;
    bool humidity_valid;

    bool hum_enabled;

    bool power_ok;
    float main_v;
    float backup_v;

    bool alarm_active;

    bool di1;
    bool di2;
    bool di3;

    bool out1;
    bool out2;
} app_logic_log_snapshot_t;

void app_logic_load_runtime_config(void);
void app_logic_handle_web_limit_update(float old_ntc_low,
                                       float old_ntc_high,
                                       float old_hum_low_limit,
                                       float old_hum_high_limit);
esp_err_t app_logic_init(void);
esp_err_t app_logic_get_log_snapshot(app_logic_log_snapshot_t *out);
static void update_display_brightness_by_power(void);
static void update_sim_led_from_modem_state(void);
