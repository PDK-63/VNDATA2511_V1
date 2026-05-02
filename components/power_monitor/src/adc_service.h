#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#include "power_monitor_types.h"

typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_main;
    adc_cali_handle_t cali_bk;
    bool cali_main_enabled;
    bool cali_bk_enabled;
} adc_service_t;

esp_err_t adc_service_init(adc_service_t *svc);
esp_err_t adc_service_read_sample(adc_service_t *svc, power_sample_t *sample);
void adc_service_deinit(adc_service_t *svc);
