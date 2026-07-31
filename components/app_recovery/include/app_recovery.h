#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_RECOVERY_BUSY_ALARM = 0,
    APP_RECOVERY_BUSY_CONFIG,
    APP_RECOVERY_BUSY_PROVISION,
    APP_RECOVERY_BUSY_MODEM_CS,
    APP_RECOVERY_BUSY_MAX,
} app_recovery_busy_reason_t;

esp_err_t app_recovery_init(void);
esp_err_t app_recovery_start(void);

void app_recovery_set_busy(app_recovery_busy_reason_t reason, bool busy);

#ifdef __cplusplus
}
#endif