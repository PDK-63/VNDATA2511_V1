#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_manager_init(void);
esp_err_t net_manager_start(void);

#ifdef __cplusplus
}
#endif