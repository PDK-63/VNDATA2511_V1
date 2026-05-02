#include "TCA9555PWR.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static QueueHandle_t gpio_evt_queue;

static esp_err_t write_reg(tca9555_t *dev, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};

    return i2c_master_write_to_device(
        dev->i2c_port,
        dev->address,
        buf,
        2,
        1000 / portTICK_PERIOD_MS);
}

static esp_err_t read_reg(tca9555_t *dev, uint8_t reg, uint8_t *data)
{
    return i2c_master_write_read_device(
        dev->i2c_port,
        dev->address,
        &reg,
        1,
        data,
        1,
        1000 / portTICK_PERIOD_MS);
}

void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// esp_err_t tca9555_read_inputs(tca9555_t *dev)
// {
//     read_reg(dev, TCA9555_INPUT_PORT0, &dev->input_state[0]);
//     read_reg(dev, TCA9555_INPUT_PORT1, &dev->input_state[1]);

//     return ESP_OK;
// }

esp_err_t tca9555_read_inputs(tca9555_t *dev)
{
    esp_err_t err;

    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    err = read_reg(dev, TCA9555_INPUT_PORT0, &dev->input_state[0]);
    if (err != ESP_OK) {
        return err;
    }

    err = read_reg(dev, TCA9555_INPUT_PORT1, &dev->input_state[1]);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static void tca9555_task(void *arg)
{
    tca9555_t *dev = (tca9555_t *)arg;

    uint32_t io_num;

    while (1)
    {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            vTaskDelay(pdMS_TO_TICKS(10)); // debounce

            tca9555_read_inputs(dev);

            printf("PORT0: %02X\n", dev->input_state[0]);
            printf("PORT1: %02X\n", dev->input_state[1]);
        }
    }
}

esp_err_t tca9555_init(tca9555_t *dev,
                       i2c_port_t port,
                       uint8_t addr,
                       gpio_num_t int_pin)
{
    dev->i2c_port = port;
    dev->address = addr;
    dev->int_pin = int_pin;

    dev->config_state[0] = 0xFF;
    dev->config_state[1] = 0xFF;
    dev->output_state[0] = 0x00;
    dev->output_state[1] = 0x00;

    write_reg(dev, TCA9555_OUTPUT_PORT0, dev->output_state[0]);
    write_reg(dev, TCA9555_OUTPUT_PORT1, dev->output_state[1]);
    write_reg(dev, TCA9555_CONFIG_PORT0, dev->config_state[0]);
    write_reg(dev, TCA9555_CONFIG_PORT1, dev->config_state[1]);

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << int_pin),
        .pull_up_en = GPIO_PULLUP_ENABLE};

    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    gpio_install_isr_service(0);
    gpio_isr_handler_add(int_pin, gpio_isr_handler, (void *)int_pin);

    xTaskCreate(tca9555_task, "tca9555_task", 4096, dev, 10, NULL);

    return ESP_OK;
}

esp_err_t tca9555_set_pin_mode(tca9555_t *dev, uint8_t pin, uint8_t mode)
{
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;

    if (mode)
        dev->config_state[port] |= (1 << bit);
    else
        dev->config_state[port] &= ~(1 << bit);

    return write_reg(dev, TCA9555_CONFIG_PORT0 + port,
                     dev->config_state[port]);
}

esp_err_t tca9555_write_pin(tca9555_t *dev, uint8_t pin, uint8_t level)
{
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;

    if (level)
        dev->output_state[port] |= (1 << bit);
    else
        dev->output_state[port] &= ~(1 << bit);

    return write_reg(dev, TCA9555_OUTPUT_PORT0 + port,
                     dev->output_state[port]);
}

// esp_err_t tca9555_read_pin(tca9555_t *dev, uint8_t pin, uint8_t *level)
// {
//     uint8_t port = pin / 8;
//     uint8_t bit = pin % 8;

//     *level = (dev->input_state[port] >> bit) & 1;

//     return ESP_OK;
// }
esp_err_t tca9555_read_pin(tca9555_t *dev, uint8_t pin, uint8_t *level)
{
    esp_err_t err;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;

    if (!dev || !level || pin > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tca9555_read_inputs(dev);
    if (err != ESP_OK) {
        return err;
    }

    *level = (dev->input_state[port] >> bit) & 1U;
    return ESP_OK;
}