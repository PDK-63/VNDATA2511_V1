#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

esp_err_t board_init(void);
void board_led_set(bool on);
bool board_led_get(void);
void board_relay_set(bool on);
bool board_relay_get(void);
esp_err_t board_modem_power_cycle(void);
esp_err_t board_modem_pwrkey_pulse(uint32_t hold_ms);
esp_err_t board_modem_reset_pulse(uint32_t hold_ms);

esp_err_t board_tca_write_pin(uint8_t pin, bool level);
esp_err_t board_tca_read_pin(uint8_t pin, bool *level);

esp_err_t board_tca_set_pin_mode(uint8_t pin, bool is_input);


esp_err_t board_gpio_input_read(gpio_num_t pin, bool *level);
esp_err_t board_tca_get_output_pin(uint8_t pin, bool *level);

esp_err_t board_modem_net_read(bool *active);
esp_err_t board_modem_sta_read(bool *active);