#pragma once

#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_manager_init(void);
esp_err_t net_manager_start(void);
bool net_manager_is_ppp_mqtt_recovering(void);
#ifdef __cplusplus
}
#endif