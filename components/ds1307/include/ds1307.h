#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS1307_I2C_ADDR  0x68

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day_of_week;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} ds1307_time_t;

/* Dung chung bus I2C da duoc project khoi tao san */
esp_err_t ds1307_init(i2c_port_t port);
esp_err_t ds1307_read_time(ds1307_time_t *t);
esp_err_t ds1307_set_time(const ds1307_time_t *t);
esp_err_t ds1307_get_time(ds1307_time_t *time);

#ifdef __cplusplus
}
#endif