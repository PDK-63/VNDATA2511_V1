#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ethernet_service_init(void);
esp_err_t ethernet_service_start(void);
esp_err_t ethernet_service_stop(void);

bool ethernet_service_is_started(void);
bool ethernet_service_is_link_up(void);
bool ethernet_service_has_ip(void);

#ifdef __cplusplus
}
#endif