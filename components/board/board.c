#include "esp_idf_version.h"
#include "board.h"
#include "app_config.h"
#include "TCA9555PWR.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_logic.h"

static const char *TAG = "board";
static bool s_led = false;
static bool s_relay = false;
static tca9555_t s_tca;
static bool s_tca_ready = false;

static void cfg_output(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, level);
}

static void cfg_input(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) return;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static esp_err_t board_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = APP_I2C_MASTER_SDA_IO,
        .scl_io_num = APP_I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = APP_I2C_MASTER_FREQ_HZ,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .clk_flags = 0,
#endif
    };
    ESP_ERROR_CHECK(i2c_param_config(APP_I2C_MASTER_PORT, &conf));
    return i2c_driver_install(APP_I2C_MASTER_PORT, conf.mode, 0, 0, 0);
}

static esp_err_t board_tca_init(void)
{
    ESP_ERROR_CHECK(tca9555_init(&s_tca, APP_I2C_MASTER_PORT, APP_TCA9555_ADDR, APP_TCA9555_INT_GPIO));
    s_tca.output_state[0] = 0x00;
    s_tca.output_state[1] = 0x00;

    cfg_input(APP_DI1_TCA_PIN);
    cfg_input(APP_DI2_TCA_PIN);
    cfg_input(APP_DI3_TCA_PIN);

    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_DO1_TCA_PIN, 0));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_DO2_TCA_PIN, 0));

    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_DI1_TCA_PIN, 1));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_DI2_TCA_PIN, 1));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_DI3_TCA_PIN, 1));
    
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_PWR, APP_MODEM_PWRKEY_IDLE_LEVEL));
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_RESET, APP_MODEM_RESET_IDLE_LEVEL));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_TCA_PIN_N_PWR, 0));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_TCA_PIN_N_RESET, 0));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_TCA_PIN_N_NET, 1));
    ESP_ERROR_CHECK(tca9555_set_pin_mode(&s_tca, APP_TCA_PIN_N_STA, 1));
    s_tca_ready = true;
    return ESP_OK;
}

esp_err_t board_init(void)
{
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_tca_init());
    return ESP_OK;
}

bool board_led_get(void)
{
    return s_led;
}

bool board_relay_get(void)
{
    return s_relay;
}

esp_err_t board_tca_write_pin(uint8_t pin, bool level)
{
    if (!s_tca_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pin > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    return tca9555_write_pin(&s_tca, pin, level ? 1 : 0);
}

esp_err_t board_tca_read_pin(uint8_t pin, bool *level)
{
    uint8_t raw = 0;
    esp_err_t err;

    if (!s_tca_ready || !level) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pin > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tca9555_read_pin(&s_tca, pin, &raw);
    if (err != ESP_OK) {
        return err;
    }

    *level = (raw != 0);
    return ESP_OK;
}

esp_err_t board_tca_set_pin_mode(uint8_t pin, bool is_input)
{
    if (!s_tca_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pin > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    return tca9555_set_pin_mode(&s_tca, pin, is_input ? 1 : 0);
}

esp_err_t board_gpio_input_read(gpio_num_t pin, bool *level)
{
    if (!level || pin == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    *level = (gpio_get_level(pin) != 0);
    return ESP_OK;
}

esp_err_t board_tca_get_output_pin(uint8_t pin, bool *level)
{
    uint8_t port;
    uint8_t bit;

    if (!s_tca_ready || !level) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pin > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    port = pin / 8;
    bit = pin % 8;

    *level = ((s_tca.output_state[port] >> bit) & 0x01U) != 0;
    return ESP_OK;
}

esp_err_t board_modem_net_read(bool *active)
{
    bool level = false;
    esp_err_t err;

    if (!active) {
        return ESP_ERR_INVALID_ARG;
    }

    err = board_tca_read_pin(APP_TCA_PIN_N_NET, &level);
    if (err != ESP_OK) {
        return err;
    }

    *active = !level;
    return ESP_OK;
}

esp_err_t board_modem_sta_read(bool *active)
{
    bool level = false;
    esp_err_t err;

    if (!active) {
        return ESP_ERR_INVALID_ARG;
    }

    err = board_tca_read_pin(APP_TCA_PIN_N_STA, &level);
    if (err != ESP_OK) {
        return err;
    }

    *active = !level;
    return ESP_OK;
}

esp_err_t board_modem_pwrkey_pulse(uint32_t hold_ms)
{
    if (!s_tca_ready) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "modem pwrkey pulse %lu ms", (unsigned long)hold_ms);
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_PWR, APP_MODEM_PWRKEY_ASSERT_LEVEL));
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_PWR, APP_MODEM_PWRKEY_IDLE_LEVEL));
    return ESP_OK;
}

esp_err_t board_modem_reset_pulse(uint32_t hold_ms)
{
    if (!s_tca_ready) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "modem reset pulse %lu ms", (unsigned long)hold_ms);
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_RESET, APP_MODEM_RESET_ASSERT_LEVEL));
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    ESP_ERROR_CHECK(tca9555_write_pin(&s_tca, APP_TCA_PIN_N_RESET, APP_MODEM_RESET_IDLE_LEVEL));
    return ESP_OK;
}

esp_err_t board_modem_power_cycle(void)
{
    esp_err_t err = board_modem_reset_pulse(APP_MODEM_RESET_PULSE_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(APP_MODEM_AFTER_RESET_WAIT_MS));
    }
    return err;
}
