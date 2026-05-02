#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char apn[32];
    char broker_uri[160];
    char mqtt_username[128];
    char mqtt_password[128];

    char wifi_ssid[64];
    char wifi_pass[64];
    bool wifi_enabled;

    int telemetry_interval_ms;

    char alarm_number1[16];
    char alarm_number2[16];
    char alarm_number3[16];
    char alarm_number4[16];
    char alarm_number5[16];
    char alarm_number6[16];

    float ntc_low_limit_c;
    float ntc_high_limit_c;
    float ntc_calib_c;

    float hum_low_limit_pct;
    float hum_high_limit_pct;
    float hum_calib_pct;
    bool hum_enabled;
    
    char message[64];
    bool gps_enabled;
} runtime_config_t;

esp_err_t runtime_config_init(void);
esp_err_t runtime_config_load(runtime_config_t *cfg);
esp_err_t runtime_config_save(const runtime_config_t *cfg);
esp_err_t runtime_config_factory_reset(void);