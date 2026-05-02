#include "adc_service.h"
#include "power_monitor_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "pm_adc";

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

static bool adc_calibration_init_for_channel(adc_unit_t unit,
                                             adc_channel_t channel,
                                             adc_atten_t atten,
                                             adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = PM_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve_cfg, &handle) == ESP_OK) {
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t line_cfg = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = PM_ADC_BITWIDTH,
        };
        if (adc_cali_create_scheme_line_fitting(&line_cfg, &handle) == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    return calibrated;
}

static void adc_calibration_deinit(adc_cali_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#endif
}

static esp_err_t adc_read_channel_mv(adc_service_t *svc,
                                     adc_channel_t channel,
                                     adc_cali_handle_t cali_handle,
                                     bool cali_enabled,
                                     int *out_mv)
{
    if (svc == NULL || out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    int64_t raw_sum = 0;

    for (int i = 0; i < PM_SAMPLE_COUNT_PER_READ; ++i) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(svc->adc_handle, channel, &raw), TAG, "adc read failed");
        raw_sum += raw;
        vTaskDelay(pdMS_TO_TICKS(PM_SAMPLE_INTERVAL_MS));
    }

    int raw_avg = (int)(raw_sum / PM_SAMPLE_COUNT_PER_READ);
    int mv = 0;

    if (cali_enabled) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(cali_handle, raw_avg, &mv), TAG, "adc cali failed");
    } else {
        mv = (raw_avg * 3300) / 4095;
    }

    *out_mv = mv;
    return ESP_OK;
}

esp_err_t adc_service_init(adc_service_t *svc)
{
    if (svc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(svc, 0, sizeof(*svc));

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = PM_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &svc->adc_handle), TAG, "new adc unit failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = PM_ADC_ATTEN,
        .bitwidth = PM_ADC_BITWIDTH,
    };

    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(svc->adc_handle, PM_ADC_MAIN_CHANNEL, &chan_cfg),
                        TAG, "config main channel failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(svc->adc_handle, PM_ADC_BK_CHANNEL, &chan_cfg),
                        TAG, "config backup channel failed");

    svc->cali_main_enabled = adc_calibration_init_for_channel(PM_ADC_UNIT,
                                                              PM_ADC_MAIN_CHANNEL,
                                                              PM_ADC_ATTEN,
                                                              &svc->cali_main);
    svc->cali_bk_enabled = adc_calibration_init_for_channel(PM_ADC_UNIT,
                                                            PM_ADC_BK_CHANNEL,
                                                            PM_ADC_ATTEN,
                                                            &svc->cali_bk);

    ESP_LOGI(TAG, "ADC init done, cal_main=%d cal_bk=%d",
             svc->cali_main_enabled, svc->cali_bk_enabled);
    return ESP_OK;
}

esp_err_t adc_service_read_sample(adc_service_t *svc, power_sample_t *sample)
{
    if (svc == NULL || sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int main_mv_pin = 0;
    int bk_mv_pin = 0;

    ESP_RETURN_ON_ERROR(adc_read_channel_mv(svc,
                                            PM_ADC_MAIN_CHANNEL,
                                            svc->cali_main,
                                            svc->cali_main_enabled,
                                            &main_mv_pin),
                        TAG, "read main failed");

    ESP_RETURN_ON_ERROR(adc_read_channel_mv(svc,
                                            PM_ADC_BK_CHANNEL,
                                            svc->cali_bk,
                                            svc->cali_bk_enabled,
                                            &bk_mv_pin),
                        TAG, "read backup failed");

    sample->main_v = ((float)main_mv_pin / 1000.0f) * PM_VOLTAGE_DIVIDER_RATIO * PM_ADC_GAIN_CAL_FACTOR;
    sample->bk_v = ((float)bk_mv_pin / 1000.0f) * PM_VOLTAGE_DIVIDER_RATIO * PM_ADC_GAIN_CAL_FACTOR;
    sample->timestamp_us = now_us();
    return ESP_OK;
}

void adc_service_deinit(adc_service_t *svc)
{
    if (svc == NULL) {
        return;
    }

    adc_calibration_deinit(svc->cali_main);
    adc_calibration_deinit(svc->cali_bk);

    if (svc->adc_handle != NULL) {
        adc_oneshot_del_unit(svc->adc_handle);
        svc->adc_handle = NULL;
    }

    memset(svc, 0, sizeof(*svc));
}
