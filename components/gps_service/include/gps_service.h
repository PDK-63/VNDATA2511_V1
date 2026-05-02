#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    double latitude;
    double longitude;
    float hdop;
    float altitude_m;
    float speed_kmh;
    int fix;          // 1=no fix, 2=2D, 3=3D
    int satellites;
    char utc_time[16];
    char utc_date[16];
} gps_location_t;

esp_err_t gps_service_init(void);
esp_err_t gps_service_start(void);
esp_err_t gps_service_stop(void);
esp_err_t gps_service_read(gps_location_t *out);
esp_err_t gps_service_get_location_once(gps_location_t *out, uint32_t timeout_ms);
bool gps_service_last_location(gps_location_t *out);
const char *gps_service_err_to_str(esp_err_t err);

#ifdef __cplusplus
}
#endif