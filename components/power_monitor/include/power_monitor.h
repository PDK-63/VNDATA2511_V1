#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "power_monitor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*power_monitor_event_cb_t)(const power_event_t *event, void *user_ctx);

esp_err_t power_monitor_init(void);
esp_err_t power_monitor_start(void);
esp_err_t power_monitor_register_callback(power_monitor_event_cb_t cb, void *user_ctx);
esp_err_t power_monitor_get_latest(power_sample_t *sample, power_state_t *state);
adc_oneshot_unit_handle_t power_monitor_get_adc_handle(void);
const char *power_monitor_state_to_str(power_state_t state);
const char *power_monitor_event_to_str(power_event_type_t type);

#ifdef __cplusplus
}
#endif