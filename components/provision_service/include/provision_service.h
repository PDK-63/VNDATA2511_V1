#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t provision_service_start(void);
esp_err_t provision_service_stop(void);
bool provision_service_is_running(void);

#ifdef __cplusplus
}
#endif
