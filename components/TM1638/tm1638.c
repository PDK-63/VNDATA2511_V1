#include "tm1638.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TM_CMD_DATA_AUTO   0x40
#define TM_CMD_DATA_FIXED  0x44
#define TM_CMD_READ_KEYS   0x42
#define TM_CMD_ADDR        0xC0
#define TM_CMD_DISPLAY     0x80

#define TM_DELAY_US        2

static const uint8_t digit_map[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F  // 9
};

static inline void tm_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

static inline bool tm_valid(const tm1638_t *dev)
{
    return (dev != NULL);
}

static void tm_dio_output(tm1638_t *dev)
{
    gpio_set_direction(dev->dio_pin, GPIO_MODE_OUTPUT);
}

static void tm_dio_input(tm1638_t *dev)
{
    gpio_set_direction(dev->dio_pin, GPIO_MODE_INPUT);
}

static void tm_write_byte(tm1638_t *dev, uint8_t data)
{
    tm_dio_output(dev);

    for (int i = 0; i < 8; i++) {
        gpio_set_level(dev->clk_pin, 0);
        gpio_set_level(dev->dio_pin, (data >> i) & 0x01);
        tm_delay_us(TM_DELAY_US);
        gpio_set_level(dev->clk_pin, 1);
        tm_delay_us(TM_DELAY_US);
    }
}

static uint8_t tm_read_byte(tm1638_t *dev)
{
    uint8_t data = 0;
    tm_dio_input(dev);

    for (int i = 0; i < 8; i++) {
        gpio_set_level(dev->clk_pin, 0);
        tm_delay_us(TM_DELAY_US);

        if (gpio_get_level(dev->dio_pin)) {
            data |= (1U << i);
        }

        gpio_set_level(dev->clk_pin, 1);
        tm_delay_us(TM_DELAY_US);
    }

    return data;
}

static void tm_send_cmd(tm1638_t *dev, uint8_t cmd)
{
    gpio_set_level(dev->stb_pin, 0);
    tm_write_byte(dev, cmd);
    gpio_set_level(dev->stb_pin, 1);
}

static esp_err_t tm_apply_display_control(tm1638_t *dev)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd = TM_CMD_DISPLAY;
    if (dev->display_on) {
        cmd |= 0x08;
    }
    cmd |= (dev->brightness & 0x07);

    tm_send_cmd(dev, cmd);
    return ESP_OK;
}

/*
 * Board của bạn đang đi theo kiểu:
 * - address 0,2,4,...14 = segment plane
 * - data bit0..bit7 = digit0..digit7
 *
 * Nghĩa là:
 * plane 0 -> segment a cho 8 digit
 * plane 1 -> segment b cho 8 digit
 * ...
 * plane 7 -> dp cho 8 digit
 */
static esp_err_t tm_write_segment_plane(tm1638_t *dev, uint8_t plane, uint8_t digit_mask)
{
    if (!tm_valid(dev) || plane >= 8) {
        return ESP_ERR_INVALID_ARG;
    }

    tm_send_cmd(dev, TM_CMD_DATA_FIXED);

    gpio_set_level(dev->stb_pin, 0);
    tm_write_byte(dev, TM_CMD_ADDR + (plane * 2));   // 0,2,4,...14
    tm_write_byte(dev, digit_mask);                  // bit0..bit7 = digit on/off
    gpio_set_level(dev->stb_pin, 1);

    return ESP_OK;
}

// static esp_err_t tm_flush(tm1638_t *dev)
// {
//     if (!tm_valid(dev)) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     for (int plane = 0; plane < 8; plane++) {
//         uint8_t digit_mask = 0;

//         for (int pos = 0; pos < 8; pos++) {
//             if (dev->digits[pos] & (1U << plane)) {
//                 digit_mask |= (1U << pos);
//             }
//         }

//         esp_err_t err = tm_write_segment_plane(dev, plane, digit_mask);
//         if (err != ESP_OK) {
//             return err;
//         }
//     }

//     return ESP_OK;
// }

// static esp_err_t tm_flush(tm1638_t *dev)
// {
//     if (!tm_valid(dev)) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     tm_send_cmd(dev, TM_CMD_DATA_AUTO);

//     gpio_set_level(dev->stb_pin, 0);
//     tm_write_byte(dev, TM_CMD_ADDR);

//     for (uint8_t i = 0; i < TM1638_NUM_DIGITS; i++) {
//         /*
//          * address chan: segment cua digit i
//          */
//         tm_write_byte(dev, dev->digits[i]);

//         /*
//          * address le: LED don i
//          * Board cua ban dang dung data 0x02 de bat LED.
//          */
//         uint8_t led_data = (dev->led_mask & (1U << i)) ? 0x02 : 0x00;
//         tm_write_byte(dev, led_data);
//     }

//     gpio_set_level(dev->stb_pin, 1);

//     return tm_apply_display_control(dev);
// }

static esp_err_t tm_write_led8_raw(tm1638_t *dev, tm1638_led8_color_t color);

static esp_err_t tm_flush(tm1638_t *dev)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Ghi 8 plane segment:
     * address 0,2,4,...14 = segment plane
     * data bit0..bit7 = digit0..digit7
     */
    for (int plane = 0; plane < 8; plane++) {
        uint8_t digit_mask = 0;

        for (int pos = 0; pos < TM1638_NUM_DIGITS; pos++) {
            if (dev->digits[pos] & (1U << plane)) {
                digit_mask |= (1U << pos);
            }
        }

        esp_err_t err = tm_write_segment_plane(dev, plane, digit_mask);
        if (err != ESP_OK) {
            return err;
        }
    }

    /*
     * Ghi lai 8 LED don theo led_mask.
     * LED0 -> address 1
     * LED1 -> address 3
     * ...
     * LED7 -> address 15
     */
    tm_send_cmd(dev, TM_CMD_DATA_FIXED);

    for (uint8_t led = 0; led < TM1638_NUM_LEDS; led++) {
        uint8_t addr = TM_CMD_ADDR + 1 + (led * 2);
        uint8_t data = 0x00;

        /* LED don */
        if (dev->led_mask & (1U << led)) {
            data |= 0x02;
        }

        /*
        * LED RGB server nam chung dia chi 0xCD / 0xCF:
        * RED   -> addr 0xCD, bit 0x01
        * GREEN -> addr 0xCF, bit 0x01
        */
        if (led == 6 &&
            (dev->led8_color == TM1638_LED8_RED ||
            dev->led8_color == TM1638_LED8_BOTH)) {
            data |= 0x01;
        }

        if (led == 7 &&
            (dev->led8_color == TM1638_LED8_GREEN ||
            dev->led8_color == TM1638_LED8_BOTH)) {
            data |= 0x01;
        }

        gpio_set_level(dev->stb_pin, 0);
        tm_write_byte(dev, addr);
        tm_write_byte(dev, data);
        gpio_set_level(dev->stb_pin, 1);
    }
     return tm_apply_display_control(dev);
}


uint8_t tm1638_encode_char(char c, bool dot)
{
    uint8_t seg = 0x00;

    if (c >= '0' && c <= '9') {
        seg = digit_map[c - '0'];
    } else {
        switch (c) {
            case ' ': seg = 0x00; break;
            case '-': seg = 0x40; break;
            case '_': seg = 0x08; break;

            case 'A':
            case 'a': seg = 0x77; break;
            case 'B':
            case 'b': seg = 0x7C; break;
            case 'C': seg = 0x39; break;
            case 'c': seg = 0x58; break;
            case 'D':
            case 'd': seg = 0x5E; break;
            case 'E':
            case 'e': seg = 0x79; break;
            case 'F':
            case 'f': seg = 0x71; break;
            case 'G':
            case 'g': seg = 0x3D; break;
            case 'H':
            case 'h': seg = 0x76; break;
            case 'I':
            case 'i': seg = 0x06; break;
            case 'J':
            case 'j': seg = 0x1E; break;
            case 'L':
            case 'l': seg = 0x38; break;
            case 'N':
            case 'n': seg = 0x54; break;
            case 'O':
            case 'o': seg = 0x5C; break;
            case 'P':
            case 'p': seg = 0x73; break;
            case 'R':
            case 'r': seg = 0x50; break;
            case 'S':
            case 's': seg = 0x6D; break;
            case 'T':
            case 't': seg = 0x78; break;
            case 'U':
            case 'u': seg = 0x3E; break;
            case 'Y':
            case 'y': seg = 0x6E; break;

            default: seg = 0x00; break;
        }
    }

    if (dot) {
        seg |= 0x80;
    }

    return seg;
}

esp_err_t tm1638_init(tm1638_t *dev,
                      gpio_num_t stb_pin,
                      gpio_num_t clk_pin,
                      gpio_num_t dio_pin,
                      uint8_t brightness)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 7) {
        brightness = 7;
    }

    dev->stb_pin = stb_pin;
    dev->clk_pin = clk_pin;
    dev->dio_pin = dio_pin;
    dev->brightness = brightness;
    dev->display_on = true;
    dev->led_mask = 0x00;
    //dev->led_mask = 0x00;
    dev->led8_color = TM1638_LED8_OFF;
    memset(dev->digits, 0, sizeof(dev->digits));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << stb_pin) | (1ULL << clk_pin) | (1ULL << dio_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(dev->stb_pin, 1);
    gpio_set_level(dev->clk_pin, 1);
    gpio_set_level(dev->dio_pin, 1);

    err = tm_apply_display_control(dev);
    if (err != ESP_OK) {
        return err;
    }

    return tm1638_clear(dev);
}

// esp_err_t tm1638_clear(tm1638_t *dev)
// {
//     if (!tm_valid(dev)) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     memset(dev->digits, 0, sizeof(dev->digits));

//     esp_err_t err = tm_flush(dev);
//     if (err != ESP_OK) {
//         return err;
//     }

//     for (int i = 0; i < 7; i++) {   // sửa chỗ này
//         err = tm1638_set_led(dev, i, false);
//         if (err != ESP_OK) {
//             return err;
//         }
//     }

//     return ESP_OK;
// }

// esp_err_t tm1638_clear(tm1638_t *dev)
// {
//     if (!tm_valid(dev)) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     memset(dev->digits, 0, sizeof(dev->digits));
//     dev->led_mask = 0x00;

//     return tm_flush(dev);
// }

esp_err_t tm1638_clear(tm1638_t *dev)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev->digits, 0, sizeof(dev->digits));
    dev->led_mask = 0x00;
    dev->led8_color = TM1638_LED8_OFF;

    return tm_flush(dev);
}

esp_err_t tm1638_set_display(tm1638_t *dev, bool on)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->display_on = on;
    return tm_apply_display_control(dev);
}

esp_err_t tm1638_set_brightness(tm1638_t *dev, uint8_t brightness)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 7) {
        brightness = 7;
    }

    dev->brightness = brightness;
    return tm_apply_display_control(dev);
}

esp_err_t tm1638_set_digit_raw(tm1638_t *dev, uint8_t pos, uint8_t seg)
{
    if (!tm_valid(dev) || pos >= TM1638_NUM_DIGITS) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->digits[pos] = seg;
    return tm_flush(dev);
}

esp_err_t tm1638_set_digit(tm1638_t *dev, uint8_t pos, int digit, bool dot)
{
    if (!tm_valid(dev) || pos >= TM1638_NUM_DIGITS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t seg = 0x00;

    if (digit >= 0 && digit <= 9) {
        seg = digit_map[digit];
    } else if (digit == -1) {
        seg = 0x00;
    } else if (digit == -2) {
        seg = 0x40;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (dot) {
        seg |= 0x80;
    }

    return tm1638_set_digit_raw(dev, pos, seg);
}

// Led 1 den 8 tuong ung voi led bit 0 den bit 7 tren TM1638
// esp_err_t tm1638_set_led(tm1638_t *dev, uint8_t led, bool on)
// {
//     if (!dev || led >= 7) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     uint8_t addr = 0xC1 + (led * 2);
//     uint8_t data = on ? 0x02 : 0x00;

//     gpio_set_level(dev->stb_pin, 0);
//     tm_write_byte(dev, 0x44);
//     gpio_set_level(dev->stb_pin, 1);

//     gpio_set_level(dev->stb_pin, 0);
//     tm_write_byte(dev, addr);
//     tm_write_byte(dev, data);
//     gpio_set_level(dev->stb_pin, 1);

//     return ESP_OK;
// }

esp_err_t tm1638_set_led(tm1638_t *dev, uint8_t led, bool on)
{
    if (!tm_valid(dev) || led >= TM1638_NUM_LEDS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (on) {
        dev->led_mask |= (1U << led);
    } else {
        dev->led_mask &= ~(1U << led);
    }

    return tm_flush(dev);
}

esp_err_t tm1638_display_text(tm1638_t *dev, const char *text)
{
    if (!tm_valid(dev) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t segs[TM1638_NUM_DIGITS] = {0};
    size_t out = 0;

    for (size_t i = 0; text[i] != '\0' && out < TM1638_NUM_DIGITS; i++) {
        if (text[i] == '.') {
            if (out > 0) {
                segs[out - 1] |= 0x80;
            }
            continue;
        }

        segs[out++] = tm1638_encode_char(text[i], false);
    }

    for (uint8_t i = 0; i < TM1638_NUM_DIGITS; i++) {
        dev->digits[i] = segs[i];
    }

    return tm_flush(dev);
}

esp_err_t tm1638_display_int(tm1638_t *dev, int value)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);

    size_t len = strlen(buf);
    if (len > TM1638_NUM_DIGITS) {
        return tm1638_display_text(dev, "--------");
    }

    char out[TM1638_NUM_DIGITS + 1];
    memset(out, ' ', TM1638_NUM_DIGITS);
    out[TM1638_NUM_DIGITS] = '\0';

    memcpy(&out[TM1638_NUM_DIGITS - len], buf, len);

    return tm1638_display_text(dev, out);
}

esp_err_t tm1638_display_float(tm1638_t *dev,
                               uint8_t start_pos,
                               uint8_t width,
                               float value,
                               uint8_t decimals,
                               bool leading_zero)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (width == 0 || start_pos >= TM1638_NUM_DIGITS || (start_pos + width) > TM1638_NUM_DIGITS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decimals > 3) {
        decimals = 3;
    }

    char fmt[8];
    char buf[32];

    while (1) {
        snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
        snprintf(buf, sizeof(buf), fmt, value);

        if (strlen(buf) <= width) {
            break;
        }

        if (decimals == 0) {
            for (uint8_t i = 0; i < width; i++) {
                dev->digits[start_pos + i] = tm1638_encode_char('-', false);
            }
            return tm_flush(dev);
        }

        decimals--;
    }

    size_t len = strlen(buf);

    char out[16];
    memset(out, leading_zero ? '0' : ' ', width);
    out[width] = '\0';

    memcpy(&out[width - len], buf, len);

    uint8_t segs[8] = {0};
    uint8_t seg_count = 0;

    for (uint8_t i = 0; i < width && out[i] != '\0'; i++) {
        if (out[i] == '.') {
            if (seg_count > 0) {
                segs[seg_count - 1] |= 0x80;
            }
            continue;
        }
        segs[seg_count++] = tm1638_encode_char(out[i], false);
    }

    while (seg_count < width) {
        segs[seg_count++] = tm1638_encode_char(' ', false);
    }

    for (uint8_t i = 0; i < width; i++) {
        dev->digits[start_pos + i] = segs[i];
    }

    return tm_flush(dev);
}

esp_err_t tm1638_display_temp_humi(tm1638_t *dev, float temp, float humi)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    err = tm1638_display_float(dev, 0, 4, temp, 1, false);
    if (err != ESP_OK) {
        return err;
    }

    err = tm1638_display_float(dev, 4, 4, humi, 1, false);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t tm1638_read_keys(tm1638_t *dev, uint8_t *keys)
{
    if (!tm_valid(dev) || keys == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *keys = 0;

    gpio_set_level(dev->stb_pin, 0);
    tm_write_byte(dev, TM_CMD_READ_KEYS);

    uint8_t raw[4];
    for (int i = 0; i < 4; i++) {
        raw[i] = tm_read_byte(dev);
    }

    gpio_set_level(dev->stb_pin, 1);
    tm_dio_output(dev);
    gpio_set_level(dev->dio_pin, 1);

    *keys =
        (((raw[0] & 0x01) ? 1 : 0) << 0) |
        (((raw[1] & 0x01) ? 1 : 0) << 1) |
        (((raw[2] & 0x01) ? 1 : 0) << 2) |
        (((raw[3] & 0x01) ? 1 : 0) << 3) |
        (((raw[0] & 0x02) ? 1 : 0) << 4) |
        (((raw[1] & 0x02) ? 1 : 0) << 5) |
        (((raw[2] & 0x02) ? 1 : 0) << 6) |
        (((raw[3] & 0x02) ? 1 : 0) << 7);

    return ESP_OK;
}


static esp_err_t tm1638_write_fixed(tm1638_t *dev, uint8_t addr, uint8_t data)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(dev->stb_pin, 0);
    tm_write_byte(dev, 0x44);   // fixed address mode
    gpio_set_level(dev->stb_pin, 1);

    gpio_set_level(dev->stb_pin, 0);
    tm_write_byte(dev, addr);
    tm_write_byte(dev, data);
    gpio_set_level(dev->stb_pin, 1);

    return ESP_OK;
}

// esp_err_t tm1638_set_led8(tm1638_t *dev, tm1638_led8_color_t color)
// {
//     if (!dev) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     // D8 nằm trên SEG9:
//     // - một màu ở GR7  -> addr 0xCD
//     // - một màu ở GR8  -> addr 0xCF
//     // SEG9 mask giả định là 0x01
//     const uint8_t seg9_mask = 0x01;
//     const uint8_t addr_g = 0xCD;   // G
//     const uint8_t addr_h = 0xCF;   // H

//     uint8_t data_g = 0x00;
//     uint8_t data_h = 0x00;

//     switch (color) {
//         case TM1638_LED8_OFF:
//             data_g = 0x00;
//             data_h = 0x00;
//             break;

//         case TM1638_LED8_RED:
//             data_g = seg9_mask;
//             data_h = 0x00;
//             break;

//         case TM1638_LED8_GREEN:
//             data_g = 0x00;
//             data_h = seg9_mask;
//             break;

//         case TM1638_LED8_BOTH:
//             data_g = seg9_mask;
//             data_h = seg9_mask;
//             break;

//         default:
//             return ESP_ERR_INVALID_ARG;
//     }

//     tm1638_write_fixed(dev, addr_g, data_g);
//     tm1638_write_fixed(dev, addr_h, data_h);

//     return ESP_OK;
// }

esp_err_t tm1638_set_led8(tm1638_t *dev, tm1638_led8_color_t color)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (color) {
    case TM1638_LED8_OFF:
    case TM1638_LED8_RED:
    case TM1638_LED8_GREEN:
    case TM1638_LED8_BOTH:
        dev->led8_color = color;
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    return tm_flush(dev);
}

static esp_err_t tm_write_led8_raw(tm1638_t *dev, tm1638_led8_color_t color)
{
    if (!tm_valid(dev)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * D8 nam tren SEG9:
     * - mot mau o GR7 -> addr 0xCD
     * - mot mau o GR8 -> addr 0xCF
     * SEG9 mask = 0x01
     */
    const uint8_t seg9_mask = 0x01;
    const uint8_t addr_g = 0xCD;
    const uint8_t addr_h = 0xCF;

    uint8_t data_g = 0x00;
    uint8_t data_h = 0x00;

    switch (color) {
    case TM1638_LED8_OFF:
        data_g = 0x00;
        data_h = 0x00;
        break;

    case TM1638_LED8_RED:
        data_g = seg9_mask;
        data_h = 0x00;
        break;

    case TM1638_LED8_GREEN:
        data_g = 0x00;
        data_h = seg9_mask;
        break;

    case TM1638_LED8_BOTH:
        data_g = seg9_mask;
        data_h = seg9_mask;
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tm1638_write_fixed(dev, addr_g, data_g);
    if (err != ESP_OK) {
        return err;
    }

    return tm1638_write_fixed(dev, addr_h, data_h);
}