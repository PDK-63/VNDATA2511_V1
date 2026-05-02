#include "ds1307.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ds1307";

static i2c_port_t s_port = I2C_NUM_0;
static bool s_inited = false;

static uint8_t dec_to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t bcd_to_dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

esp_err_t ds1307_init(i2c_port_t port)
{
    s_port = port;
    s_inited = true;
    ESP_LOGI(TAG, "DS1307 attached on I2C port=%d", port);
    return ESP_OK;
}

esp_err_t ds1307_read_time(ds1307_time_t *t)
{
    if (!s_inited || !t) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = 0x00;
    uint8_t data[7] = {0};

    esp_err_t err = i2c_master_write_read_device(
        s_port,
        DS1307_I2C_ADDR,
        &reg, 1,
        data, sizeof(data),
        pdMS_TO_TICKS(1000)
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read time failed: %s", esp_err_to_name(err));
        return err;
    }

    t->second      = bcd_to_dec(data[0] & 0x7F);
    t->minute      = bcd_to_dec(data[1] & 0x7F);
    t->hour        = bcd_to_dec(data[2] & 0x3F);
    t->day_of_week = bcd_to_dec(data[3] & 0x07);
    t->day         = bcd_to_dec(data[4] & 0x3F);
    t->month       = bcd_to_dec(data[5] & 0x1F);
    t->year        = 2000 + bcd_to_dec(data[6]);

    return ESP_OK;
}

esp_err_t ds1307_set_time(const ds1307_time_t *t)
{
    if (!s_inited || !t) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[8];
    data[0] = 0x00;
    data[1] = dec_to_bcd(t->second % 60) & 0x7F;
    data[2] = dec_to_bcd(t->minute % 60);
    data[3] = dec_to_bcd(t->hour % 24);
    data[4] = dec_to_bcd((t->day_of_week >= 1 && t->day_of_week <= 7) ? t->day_of_week : 1);
    data[5] = dec_to_bcd(t->day);
    data[6] = dec_to_bcd(t->month);
    data[7] = dec_to_bcd((uint8_t)(t->year % 100));

    esp_err_t err = i2c_master_write_to_device(
        s_port,
        DS1307_I2C_ADDR,
        data, sizeof(data),
        pdMS_TO_TICKS(1000)
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set time failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t ds1307_get_time(ds1307_time_t *time)
{
    if (!time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg = 0x00;
    uint8_t data[7] = {0};

    esp_err_t err = i2c_master_write_read_device(
        s_port,
        DS1307_I2C_ADDR,
        &reg,
        1,
        data,
        sizeof(data),
        pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK) {
        return err;
    }

    time->second = bcd_to_dec(data[0] & 0x7F);
    time->minute = bcd_to_dec(data[1] & 0x7F);
    time->hour   = bcd_to_dec(data[2] & 0x3F);

    time->day_of_week = bcd_to_dec(data[3] & 0x07);
    time->day         = bcd_to_dec(data[4] & 0x3F);
    time->month       = bcd_to_dec(data[5] & 0x1F);
    time->year        = 2000 + bcd_to_dec(data[6]);

    return ESP_OK;
}