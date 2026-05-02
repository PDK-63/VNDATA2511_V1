// #ifndef SHT30_H
// #define SHT30_H

// #include <stdint.h>
// #include <stddef.h>
// #include <string.h>

// #include "esp_err.h"
// #include "esp_log.h"
// //#include "driver/i2c_master.h"
// #include "driver/i2c.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"

// #define CommandLength 2

// typedef enum {
//     ok = 0,
//     error,
//     init_error,
//     data_not_ready,
//     data_not_valid
// } sht30_status_t;

// typedef enum {
//     Repeatability_High = 0,
//     Repeatability_Medium,
//     Repeatability_Low
// } sht30_repeatability_t;

// typedef enum {
//     ClockStretching_Enable = 0,
//     ClockStretching_Disable
// } sht30_clock_stretching_t;

// typedef enum {
//     MPS_05 = 0,
//     MPS_1,
//     MPS_2,
//     MPS_4,
//     MPS_10
// } sht30_measurements_per_seconds_t;

// typedef enum {
//     Heater_Disable = 0,
//     Heater_Enable
// } sht30_heater_t;

// typedef struct {
//     i2c_master_bus_handle_t bus_handle;
//     i2c_master_dev_handle_t dev_handle;
//     uint16_t temperature;
//     uint16_t humidity;
// } sht30_t;

// /* Single shot commands */
// #define SingleShot_CS      0x2C
// #define SingleShot_DCS     0x24

// #define SingleShot_RH_CS   0x06
// #define SingleShot_RM_CS   0x0D
// #define SingleShot_RL_CS   0x10

// #define SingleShot_RH_DCS  0x00
// #define SingleShot_RM_DCS  0x0B
// #define SingleShot_RL_DCS  0x16

// /* Periodic measurement commands */
// #define Periodic_05        0x20
// #define Periodic_1         0x21
// #define Periodic_2         0x22
// #define Periodic_4         0x23
// #define Periodic_10        0x27

// #define Periodic_RH_05     0x32
// #define Periodic_RM_05     0x24
// #define Periodic_RL_05     0x2F

// #define Periodic_RH_1      0x30
// #define Periodic_RM_1      0x26
// #define Periodic_RL_1      0x2D

// #define Periodic_RH_2      0x36
// #define Periodic_RM_2      0x20
// #define Periodic_RL_2      0x2B

// #define Periodic_RH_4      0x34
// #define Periodic_RM_4      0x22
// #define Periodic_RL_4      0x29

// #define Periodic_RH_10     0x37
// #define Periodic_RM_10     0x21
// #define Periodic_RL_10     0x2A

// /* Other commands */
// #define FetchCommand       0xE000
// #define ARTCommand         0x2B32
// #define BreakCommand       0x3093
// #define SoftResetCommand   0x30A2

// #define Heater             0x30
// #define HeaterEnable       0x6D
// #define HeaterDisable      0x66

// #define StatusRegister     0xF32D
// #define ClrStatusRegister  0x3041

// sht30_status_t sht30_init(sht30_t *sht30,
//                           uint8_t _i2c_port,
//                           uint8_t _scl_io_num,
//                           uint8_t _sda_io_num,
//                           uint8_t _device_address,
//                           uint16_t _scl_speed_hz,
//                           uint32_t _scl_wait_us);

// sht30_status_t sht30_deinit(sht30_t *sht30);

// sht30_status_t sht30_write(sht30_t *sht30, uint8_t *command);
// sht30_status_t sht30_read(sht30_t *sht30, uint8_t *dataRec, size_t len);

// sht30_status_t sht30_single_shot(sht30_t *sht30,
//                                  sht30_repeatability_t repeatability,
//                                  sht30_clock_stretching_t clock);

// sht30_status_t sht30_periodic(sht30_t *sht30,
//                               sht30_repeatability_t repeatability,
//                               sht30_measurements_per_seconds_t mps);

// sht30_status_t sht30_fetch_data(sht30_t *sht30);
// sht30_status_t sht30_art(sht30_t *sht30);
// sht30_status_t sht30_break(sht30_t *sht30);
// sht30_status_t sht30_soft_reset(sht30_t *sht30);
// sht30_status_t sht30_heater_control(sht30_t *sht30, sht30_heater_t control);
// sht30_status_t sht30_read_status_register(sht30_t *sht30);
// sht30_status_t sht30_clear_status_register(sht30_t *sht30);

// uint8_t sht30_calculate_crc(sht30_t *sht30, uint8_t *data);

// float sht30_read_temperature_celsius(sht30_t *sht30);
// float sht30_read_temperature_fahrenheit(sht30_t *sht30);
// float sht30_read_humidity(sht30_t *sht30);

// #endif /* SHT30_H */

#ifndef SHT30_H
#define SHT30_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CommandLength 2

typedef enum {
    ok = 0,
    error,
    init_error,
    data_not_ready,
    data_not_valid
} sht30_status_t;

typedef enum {
    Repeatability_High = 0,
    Repeatability_Medium,
    Repeatability_Low
} sht30_repeatability_t;

typedef enum {
    ClockStretching_Enable = 0,
    ClockStretching_Disable
} sht30_clock_stretching_t;

typedef enum {
    MPS_05 = 0,
    MPS_1,
    MPS_2,
    MPS_4,
    MPS_10
} sht30_measurements_per_seconds_t;

typedef enum {
    Heater_Disable = 0,
    Heater_Enable
} sht30_heater_t;

typedef struct {
    i2c_port_t port;
    gpio_num_t scl_io_num;
    gpio_num_t sda_io_num;
    uint8_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    bool driver_installed;
    uint16_t temperature;
    uint16_t humidity;
} sht30_t;

/* Single shot commands */
#define SingleShot_CS      0x2C
#define SingleShot_DCS     0x24

#define SingleShot_RH_CS   0x06
#define SingleShot_RM_CS   0x0D
#define SingleShot_RL_CS   0x10

#define SingleShot_RH_DCS  0x00
#define SingleShot_RM_DCS  0x0B
#define SingleShot_RL_DCS  0x16

/* Periodic measurement commands */
#define Periodic_05        0x20
#define Periodic_1         0x21
#define Periodic_2         0x22
#define Periodic_4         0x23
#define Periodic_10        0x27

#define Periodic_RH_05     0x32
#define Periodic_RM_05     0x24
#define Periodic_RL_05     0x2F

#define Periodic_RH_1      0x30
#define Periodic_RM_1      0x26
#define Periodic_RL_1      0x2D

#define Periodic_RH_2      0x36
#define Periodic_RM_2      0x20
#define Periodic_RL_2      0x2B

#define Periodic_RH_4      0x34
#define Periodic_RM_4      0x22
#define Periodic_RL_4      0x29

#define Periodic_RH_10     0x37
#define Periodic_RM_10     0x21
#define Periodic_RL_10     0x2A

/* Other commands */
#define FetchCommand       0xE000
#define ARTCommand         0x2B32
#define BreakCommand       0x3093
#define SoftResetCommand   0x30A2

#define Heater             0x30
#define HeaterEnable       0x6D
#define HeaterDisable      0x66

#define StatusRegister     0xF32D
#define ClrStatusRegister  0x3041

sht30_status_t sht30_init(sht30_t *sht30,
                          i2c_port_t _i2c_port,
                          gpio_num_t _scl_io_num,
                          gpio_num_t _sda_io_num,
                          uint8_t _device_address,
                          uint32_t _scl_speed_hz,
                          uint32_t _scl_wait_us);

sht30_status_t sht30_deinit(sht30_t *sht30);

sht30_status_t sht30_write(sht30_t *sht30, uint8_t *command);
sht30_status_t sht30_read(sht30_t *sht30, uint8_t *dataRec, size_t len);

sht30_status_t sht30_single_shot(sht30_t *sht30,
                                 sht30_repeatability_t repeatability,
                                 sht30_clock_stretching_t clock);

sht30_status_t sht30_periodic(sht30_t *sht30,
                              sht30_repeatability_t repeatability,
                              sht30_measurements_per_seconds_t mps);

sht30_status_t sht30_fetch_data(sht30_t *sht30);
sht30_status_t sht30_art(sht30_t *sht30);
sht30_status_t sht30_break(sht30_t *sht30);
sht30_status_t sht30_soft_reset(sht30_t *sht30);
sht30_status_t sht30_heater_control(sht30_t *sht30, sht30_heater_t control);
sht30_status_t sht30_read_status_register(sht30_t *sht30);
sht30_status_t sht30_clear_status_register(sht30_t *sht30);

uint8_t sht30_calculate_crc(sht30_t *sht30, uint8_t *data);

float sht30_read_temperature_celsius(sht30_t *sht30);
float sht30_read_temperature_fahrenheit(sht30_t *sht30);
float sht30_read_humidity(sht30_t *sht30);

#endif /* SHT30_H */