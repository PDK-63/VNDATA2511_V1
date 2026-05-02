#ifndef TCA9555PWR_H
#define TCA9555PWR_H

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>

#define TCA9555_INPUT_PORT0  0x00
#define TCA9555_INPUT_PORT1  0x01
#define TCA9555_OUTPUT_PORT0 0x02
#define TCA9555_OUTPUT_PORT1 0x03
#define TCA9555_CONFIG_PORT0 0x06
#define TCA9555_CONFIG_PORT1 0x07

typedef struct
{
    i2c_port_t i2c_port;
    uint8_t address;
    gpio_num_t int_pin;

    uint8_t input_state[2];
    uint8_t output_state[2];
    uint8_t config_state[2];

} tca9555_t;

esp_err_t tca9555_init(tca9555_t *dev,
                       i2c_port_t port,
                       uint8_t addr,
                       gpio_num_t int_pin);

esp_err_t tca9555_set_pin_mode(tca9555_t *dev, uint8_t pin, uint8_t mode);

esp_err_t tca9555_write_pin(tca9555_t *dev, uint8_t pin, uint8_t level);

esp_err_t tca9555_read_pin(tca9555_t *dev, uint8_t pin, uint8_t *level);

esp_err_t tca9555_read_inputs(tca9555_t *dev);

#endif