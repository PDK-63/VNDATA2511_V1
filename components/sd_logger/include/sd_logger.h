#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_logger_init(void);
esp_err_t sd_logger_start(void);
esp_err_t sd_logger_deinit(void);

bool sd_logger_is_mounted(void);
bool sd_logger_card_inserted(void);

#ifdef __cplusplus
}
#endif