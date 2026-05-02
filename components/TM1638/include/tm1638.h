#ifndef TM1638_H
#define TM1638_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TM1638_NUM_DIGITS 8
#define TM1638_NUM_LEDS   8

typedef enum {
    TM1638_LED8_OFF = 0,
    TM1638_LED8_RED,
    TM1638_LED8_GREEN,
    TM1638_LED8_BOTH
} tm1638_led8_color_t;

typedef struct {
    gpio_num_t stb_pin;
    gpio_num_t clk_pin;
    gpio_num_t dio_pin;
    uint8_t brightness;
    bool display_on;

    uint8_t led_mask;  
    tm1638_led8_color_t led8_color;
    // framebuffer cho 8 digit
    uint8_t digits[TM1638_NUM_DIGITS];
} tm1638_t;


esp_err_t tm1638_init(tm1638_t *dev,
                      gpio_num_t stb_pin,
                      gpio_num_t clk_pin,
                      gpio_num_t dio_pin,
                      uint8_t brightness);

esp_err_t tm1638_clear(tm1638_t *dev);
esp_err_t tm1638_set_display(tm1638_t *dev, bool on);
esp_err_t tm1638_set_brightness(tm1638_t *dev, uint8_t brightness);

esp_err_t tm1638_set_digit_raw(tm1638_t *dev, uint8_t pos, uint8_t seg);
esp_err_t tm1638_set_digit(tm1638_t *dev, uint8_t pos, int digit, bool dot);
esp_err_t tm1638_set_led(tm1638_t *dev, uint8_t led, bool on);

esp_err_t tm1638_display_text(tm1638_t *dev, const char *text);
esp_err_t tm1638_display_int(tm1638_t *dev, int value);
esp_err_t tm1638_display_float(tm1638_t *dev,
                               uint8_t start_pos,
                               uint8_t width,
                               float value,
                               uint8_t decimals,
                               bool leading_zero);
esp_err_t tm1638_display_temp_humi(tm1638_t *dev, float temp, float humi);

esp_err_t tm1638_read_keys(tm1638_t *dev, uint8_t *keys);

uint8_t tm1638_encode_char(char c, bool dot);
esp_err_t tm1638_set_led8(tm1638_t *dev, tm1638_led8_color_t color);
esp_err_t tm1638_set_led8_raw(tm1638_t *dev, uint8_t data);
#ifdef __cplusplus
}
#endif

#endif // TM1638_H