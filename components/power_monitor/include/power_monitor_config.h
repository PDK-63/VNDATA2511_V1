#pragma once

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

// ADC mapping for ESP32 classic
#define PM_ADC_MAIN_CHANNEL             ADC_CHANNEL_3   
#define PM_ADC_BK_CHANNEL               ADC_CHANNEL_4   
#define PM_ADC_ATTEN                    ADC_ATTEN_DB_12
#define PM_ADC_UNIT                     ADC_UNIT_1
#define PM_ADC_BITWIDTH                 ADC_BITWIDTH_DEFAULT

// Voltage divider: (100k + 10k) / 10k = 11
#define PM_VOLTAGE_DIVIDER_RATIO        11.0f
#define PM_ADC_GAIN_CAL_FACTOR          1.000f

// Sampling
#define PM_SAMPLE_COUNT_PER_READ        32
#define PM_SAMPLE_INTERVAL_MS           2
#define PM_MONITOR_PERIOD_MS            1000

// Thresholds
#define PM_MAIN_LOST_THRESHOLD_V        7.0f
#define PM_MAIN_RESTORE_THRESHOLD_V     8.0f
#define PM_MAIN_DEBOUNCE_COUNT          5
#define PM_BK_LOW_THRESHOLD_V           10.5f
#define PM_BK_PRESENT_THRESHOLD_V       6.0f

// Queue / tasks
#define PM_EVENT_QUEUE_LEN              16
#define PM_MONITOR_TASK_STACK           4096
#define PM_MONITOR_TASK_PRIORITY        5
#define PM_NETWORK_TASK_STACK           4096
#define PM_NETWORK_TASK_PRIORITY        4

#ifdef __cplusplus
}
#endif
