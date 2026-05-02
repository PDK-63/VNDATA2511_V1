#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "ntc_driver.h"

#define ADC_SAMPLES                  15
#define ADC_OUTLIER_THRESHOLD        40
#define ADC_EMA_ALPHA                0.35f
#define VCC                          3.3f

#define NTC_R_SHORT_THRESHOLD_OHM    100.0f
#define NTC_R_OPEN_THRESHOLD_OHM     150000.0f

#define NTC_TEMP_MIN_C               (-40.0f)
#define NTC_TEMP_MAX_C               (125.0f)

#define NTC_FAULT_CONFIRM_COUNT      3
#define NTC_OK_CONFIRM_COUNT         2

static const char *TAG = "NTC";

static int cmp_int(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

static int median_of_ints(const int *arr, int size)
{
    int tmp[ADC_SAMPLES];

    if (arr == NULL || size <= 0) {
        return 0;
    }

    if (size > ADC_SAMPLES) {
        size = ADC_SAMPLES;
    }

    for (int i = 0; i < size; i++) {
        tmp[i] = arr[i];
    }

    qsort(tmp, size, sizeof(int), cmp_int);

    if ((size & 1) != 0) {
        return tmp[size / 2];
    }

    return (tmp[(size / 2) - 1] + tmp[size / 2]) / 2;
}

static bool ntc_adc_calibration_init(adc_unit_t unit,
                                     adc_channel_t channel,
                                     adc_atten_t atten,
                                     adc_bitwidth_t bitwidth,
                                     adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &handle) == ESP_OK) {
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = bitwidth,
        };
        if (adc_cali_create_scheme_line_fitting(&cfg, &handle) == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    return calibrated;
}

static void ntc_adc_calibration_deinit(adc_cali_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (adc_cali_delete_scheme_curve_fitting(handle) == ESP_OK) {
        return;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    (void)adc_cali_delete_scheme_line_fitting(handle);
#endif
}

static void ntc_register_fault(ntc_t *ntc, ntc_status_t fault)
{
    if (ntc == NULL) {
        return;
    }

    ntc->ok_count = 0;

    if (fault == NTC_STATUS_OPEN_CIRCUIT ||
        fault == NTC_STATUS_SHORT_CIRCUIT) {
        ntc->fault_count = NTC_FAULT_CONFIRM_COUNT;
        ntc->status = fault;
        return;
    }

    if (ntc->fault_count < 255) {
        ntc->fault_count++;
    }

    if (ntc->fault_count >= NTC_FAULT_CONFIRM_COUNT) {
        ntc->status = fault;
    }
}

static void ntc_register_ok(ntc_t *ntc)
{
    if (ntc == NULL) {
        return;
    }

    ntc->fault_count = 0;

    if (ntc->ok_count < 255) {
        ntc->ok_count++;
    }

    if (ntc->ok_count >= NTC_OK_CONFIRM_COUNT) {
        ntc->status = NTC_STATUS_OK;
    }
}

/* Trả về 2 đường ADC:
 * - out_raw_fast: giá trị nhanh để detect lỗi sensor
 * - out_raw_filtered: giá trị đã EMA để tính nhiệt độ ổn định
 */
static esp_err_t ntc_adc_read(ntc_t *ntc, uint32_t *out_raw_fast, uint32_t *out_raw_filtered)
{
    if (ntc == NULL || out_raw_fast == NULL || out_raw_filtered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ntc->adc_handle == NULL) {
        ESP_LOGE(TAG, "adc_handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    int samples[ADC_SAMPLES];
    int valid_samples = 0;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(ntc->adc_handle, ntc->channel, &raw);
        if (err == ESP_OK) {
            samples[valid_samples++] = raw;
        }
    }

    if (valid_samples <= 0) {
        ESP_LOGE(TAG, "adc read failed: no valid samples");
        return ESP_FAIL;
    }

    int median = median_of_ints(samples, valid_samples);

    int32_t sum = 0;
    int count = 0;

    for (int i = 0; i < valid_samples; i++) {
        if (abs(samples[i] - median) <= ADC_OUTLIER_THRESHOLD) {
            sum += samples[i];
            count++;
        }
    }

    float raw_fast = (count > 0) ? ((float)sum / (float)count) : (float)median;

    if (ntc->adc_first_read) {
        ntc->adc_filtered = raw_fast;
        ntc->adc_first_read = 0;
    } else {
        ntc->adc_filtered = (ADC_EMA_ALPHA * raw_fast) +
                            ((1.0f - ADC_EMA_ALPHA) * ntc->adc_filtered);
    }

    *out_raw_fast = (uint32_t)(raw_fast + 0.5f);
    *out_raw_filtered = (uint32_t)(ntc->adc_filtered + 0.5f);
    return ESP_OK;
}

void ntc_init(ntc_t *ntc, adc_channel_t channel, adc_oneshot_unit_handle_t adc_handle)
{
    if (ntc == NULL) {
        ESP_LOGE(TAG, "ntc_init: ntc is NULL");
        return;
    }

    memset(ntc, 0, sizeof(*ntc));

    ntc->channel = channel;
    ntc->adc_handle = adc_handle;

    ntc->r_pullup = 10000.0f;
    ntc->beta = 3950.0f;
    ntc->r0 = 10000.0f;
    ntc->t0 = 298.15f;

    ntc->alpha = 0.30f;
    ntc->temp_filtered = 25.0f;
    ntc->adc_filtered = 0.0f;

    ntc->adc_first_read = 1;
    ntc->temp_first_read = 1;
    ntc->status = NTC_STATUS_ADC_ERROR;

    if (ntc->adc_handle == NULL) {
        ESP_LOGE(TAG, "ntc_init failed: adc_handle is NULL");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(ntc->adc_handle, channel, &chan_cfg));

    ntc->cali_enabled = ntc_adc_calibration_init(ADC_UNIT_1,
                                                 channel,
                                                 ADC_ATTEN_DB_12,
                                                 ADC_BITWIDTH_12,
                                                 &ntc->cali_handle);

    ESP_LOGI(TAG, "NTC init done, cali=%d channel=%d", ntc->cali_enabled ? 1 : 0, channel);
}

void ntc_deinit(ntc_t *ntc)
{
    if (ntc == NULL) {
        return;
    }

    if (ntc->cali_handle != NULL) {
        ntc_adc_calibration_deinit(ntc->cali_handle);
        ntc->cali_handle = NULL;
    }

    ntc->cali_enabled = false;
}

float ntc_read_temperature(ntc_t *ntc)
{
    if (ntc == NULL) {
        return NAN;
    }

    uint32_t adc_raw_fast = 0;
    uint32_t adc_raw_filtered = 0;

    if (ntc_adc_read(ntc, &adc_raw_fast, &adc_raw_filtered) != ESP_OK) {
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    ntc->last_adc_raw = adc_raw_fast;

    int voltage_mv_fast = 0;
    int voltage_mv_filtered = 0;

    if (ntc->cali_enabled) {
        if (adc_cali_raw_to_voltage(ntc->cali_handle, adc_raw_fast, &voltage_mv_fast) != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage fast failed");
            ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
            return NAN;
        }

        if (adc_cali_raw_to_voltage(ntc->cali_handle, adc_raw_filtered, &voltage_mv_filtered) != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage filtered failed");
            ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
            return NAN;
        }
    } else {
        voltage_mv_fast = (int)((adc_raw_fast * 3300UL) / 4095UL);
        voltage_mv_filtered = (int)((adc_raw_filtered * 3300UL) / 4095UL);
    }

    ntc->last_voltage_mv = voltage_mv_fast;

    float v_fast = voltage_mv_fast / 1000.0f;
    float v_filtered = voltage_mv_filtered / 1000.0f;

    if (adc_raw_fast >= 4090 || v_fast >= 3.25f) {
        ESP_LOGW(TAG, "NTC open/unplugged hard detect: adc=%lu mv=%d v=%.3f",
                (unsigned long)adc_raw_fast, voltage_mv_fast, v_fast);
        ntc_register_fault(ntc, NTC_STATUS_OPEN_CIRCUIT);
        return NAN;
    }

    if (adc_raw_fast <= 5 || v_fast <= 0.02f) {
        ESP_LOGW(TAG, "NTC short hard detect: adc=%lu mv=%d v=%.3f",
                (unsigned long)adc_raw_fast, voltage_mv_fast, v_fast);
        ntc_register_fault(ntc, NTC_STATUS_SHORT_CIRCUIT);
        return NAN;
    }
    /* Detect lỗi bằng đường nhanh */
    if (v_fast <= 0.0f || v_fast >= VCC) {
        ESP_LOGW(TAG, "invalid voltage: adc=%lu mv=%d v=%.3f",
                 (unsigned long)adc_raw_fast, voltage_mv_fast, v_fast);
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    float r_ntc_fast = ntc->r_pullup * v_fast / (VCC - v_fast);

    if (!isfinite(r_ntc_fast) || r_ntc_fast <= 0.0f) {
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    if (r_ntc_fast < NTC_R_SHORT_THRESHOLD_OHM) {
        ESP_LOGW(TAG, "NTC short: adc=%lu mv=%d r_ntc=%.1f ohm",
                 (unsigned long)adc_raw_fast, voltage_mv_fast, r_ntc_fast);
        ntc_register_fault(ntc, NTC_STATUS_SHORT_CIRCUIT);
        return NAN;
    }

    if (r_ntc_fast > NTC_R_OPEN_THRESHOLD_OHM) {
        ESP_LOGW(TAG, "NTC open/unplugged: adc=%lu mv=%d r_ntc=%.1f ohm",
                 (unsigned long)adc_raw_fast, voltage_mv_fast, r_ntc_fast);
        ntc_register_fault(ntc, NTC_STATUS_OPEN_CIRCUIT);
        return NAN;
    }

    /* Tính nhiệt độ bằng đường đã lọc */
    if (v_filtered <= 0.0f || v_filtered >= VCC) {
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    float r_ntc = ntc->r_pullup * v_filtered / (VCC - v_filtered);
    ntc->last_r_ntc = r_ntc;

    if (!isfinite(r_ntc) || r_ntc <= 0.0f) {
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    float temp_k = 1.0f / ((1.0f / ntc->t0) +
                           (1.0f / ntc->beta) * logf(r_ntc / ntc->r0));

    float temp_c = temp_k - 273.15f;
    ntc->last_temp_raw = temp_c;

    if (!isfinite(temp_c)) {
        ntc_register_fault(ntc, NTC_STATUS_ADC_ERROR);
        return NAN;
    }

    if (temp_c < NTC_TEMP_MIN_C || temp_c > NTC_TEMP_MAX_C) {
        ESP_LOGW(TAG, "temperature out of range: %.2f C", temp_c);
        ntc_register_fault(ntc, NTC_STATUS_OUT_OF_RANGE);
        return NAN;
    }

    if (ntc->temp_first_read) {
        ntc->temp_filtered = temp_c;
        ntc->temp_first_read = 0;
    } else {
        ntc->temp_filtered += ntc->alpha * (temp_c - ntc->temp_filtered);
    }

    ntc_register_ok(ntc);

    ESP_LOGD(TAG, "adc_fast=%lu adc_filt=%lu mv_fast=%d mv_filt=%d r_fast=%.1f r_filt=%.1f temp_raw=%.2f temp_f=%.2f status=%d",
             (unsigned long)adc_raw_fast,
             (unsigned long)adc_raw_filtered,
             voltage_mv_fast,
             voltage_mv_filtered,
             r_ntc_fast,
             r_ntc,
             temp_c,
             ntc->temp_filtered,
             ntc->status);

    return ntc->temp_filtered;
}

ntc_status_t ntc_get_status(const ntc_t *ntc)
{
    if (ntc == NULL) {
        return NTC_STATUS_ADC_ERROR;
    }

    return ntc->status;
}

bool ntc_is_sensor_fault(const ntc_t *ntc)
{
    if (ntc == NULL) {
        return true;
    }

    return (ntc->status == NTC_STATUS_OPEN_CIRCUIT) ||
           (ntc->status == NTC_STATUS_SHORT_CIRCUIT);
}

const char *ntc_status_to_string(ntc_status_t status)
{
    switch (status) {
    case NTC_STATUS_OK:
        return "OK";
    case NTC_STATUS_OPEN_CIRCUIT:
        return "OPEN_CIRCUIT";
    case NTC_STATUS_SHORT_CIRCUIT:
        return "SHORT_CIRCUIT";
    case NTC_STATUS_ADC_ERROR:
        return "ADC_ERROR";
    case NTC_STATUS_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    default:
        return "UNKNOWN";
    }
}