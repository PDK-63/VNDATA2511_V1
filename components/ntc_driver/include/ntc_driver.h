#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

typedef enum {
    NTC_STATUS_OK = 0,
    NTC_STATUS_OPEN_CIRCUIT,
    NTC_STATUS_SHORT_CIRCUIT,
    NTC_STATUS_ADC_ERROR,
    NTC_STATUS_OUT_OF_RANGE,
} ntc_status_t;

typedef struct {
    adc_channel_t channel;
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool cali_enabled;

    float r_pullup;
    float beta;
    float r0;
    float t0;

    float alpha;
    float temp_filtered;
    float adc_filtered;

    uint8_t adc_first_read;
    uint8_t temp_first_read;

    ntc_status_t status;
    uint8_t fault_count;
    uint8_t ok_count;

    uint32_t last_adc_raw;
    int last_voltage_mv;
    float last_r_ntc;
    float last_temp_raw;
} ntc_t;

void ntc_init(ntc_t *ntc, adc_channel_t channel, adc_oneshot_unit_handle_t adc_handle);
void ntc_deinit(ntc_t *ntc);

float ntc_read_temperature(ntc_t *ntc);

ntc_status_t ntc_get_status(const ntc_t *ntc);
const char *ntc_status_to_string(ntc_status_t status);
bool ntc_is_sensor_fault(const ntc_t *ntc);