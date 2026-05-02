// #pragma once

// #include "esp_err.h"
// #include <stdbool.h>

// #ifdef __cplusplus
// extern "C" {
// #endif

// esp_err_t wifi_service_init(void);
// esp_err_t wifi_service_start_sta(const char *ssid, const char *pass);
// esp_err_t wifi_service_stop_sta(void);
// bool wifi_service_is_connected(void);

// #ifdef __cplusplus
// }
// #endif

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_service_init(void);
esp_err_t wifi_service_start_sta(const char *ssid, const char *pass);
esp_err_t wifi_service_stop_sta(void);
void wifi_service_set_reconnect_enabled(bool enabled);
bool wifi_service_is_connected(void);