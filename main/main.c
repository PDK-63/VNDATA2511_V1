#include "app_config.h"
#include "app_logic.h"
#include "board.h"
#include "diag_service.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "modem_service.h"
#include "mqtt_service.h"
#include "net_manager.h"
#include "nvs_flash.h"
#include "runtime_config.h"
#include "system_supervisor.h"
#include "wifi_service.h"
#include "power_monitor.h"
#include "ethernet_service.h"
#include <string.h>
#include "modem_status.h"
#include "status_led.h"
#include "sd_logger.h"

static const char *TAG = "main";

static void init_nvs_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    runtime_config_t cfg;

    init_nvs_or_recover();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(runtime_config_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(diag_service_init());
    ESP_ERROR_CHECK(wifi_service_init());

    modem_service_config_t modem_cfg = {
        .uart_port = APP_MODEM_UART_PORT,
        .tx_pin = APP_MODEM_UART_TX,
        .rx_pin = APP_MODEM_UART_RX,
        .baudrate = APP_MODEM_BAUDRATE,
        .apn = APP_MODEM_APN,
    };
    ESP_ERROR_CHECK(modem_service_init(&modem_cfg));

    if (runtime_config_load(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.telemetry_interval_ms = APP_DEFAULT_PUB_MS;
    }

    mqtt_service_config_t mqtt_cfg = {
        .broker_uri = (cfg.broker_uri[0] != '\0') ? cfg.broker_uri : APP_MQTT_URI,
        .username = (cfg.mqtt_username[0] != '\0') ? cfg.mqtt_username : APP_MQTT_USERNAME,
        .password = (cfg.mqtt_password[0] != '\0') ? cfg.mqtt_password : APP_MQTT_PASSWORD,
        .birth_payload = APP_MQTT_BIRTH_PAYLOAD,
        .lwt_payload = APP_MQTT_LWT_PAYLOAD,
    };
    ESP_ERROR_CHECK(mqtt_service_init(&mqtt_cfg));

    ESP_ERROR_CHECK(system_supervisor_init());

    /* Khoi tao power monitor truoc de app_logic va NTC dung chung ADC */
    ESP_ERROR_CHECK(power_monitor_init());
    ESP_ERROR_CHECK(power_monitor_start());

    ESP_ERROR_CHECK(app_logic_init());
    esp_err_t sd_err = sd_logger_start();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "sd_logger_start failed: %s", esp_err_to_name(sd_err));
    }
    ESP_ERROR_CHECK(net_manager_init());
    ESP_ERROR_CHECK(system_supervisor_start());
    ESP_ERROR_CHECK(net_manager_start());

    ESP_LOGI(TAG, "%s started, device_id=%s", APP_PROJECT_NAME, APP_DEVICE_ID);
}

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "board.h"

// static const char *TAG = "main";

// void app_main(void)
// {
//     ESP_ERROR_CHECK(board_init());

//     ESP_ERROR_CHECK(board_tca_set_pin_mode(1, false));   // P01 = OUTPUT1
//     ESP_ERROR_CHECK(board_tca_set_pin_mode(0, false));   // P00 = OUTPUT2

//     while (1) {
//         ESP_LOGI(TAG, "TEST 1: DO1 ON, DO2 OFF");
//         ESP_ERROR_CHECK(board_tca_write_pin(1, true));
//         ESP_ERROR_CHECK(board_tca_write_pin(0, false));
//         vTaskDelay(pdMS_TO_TICKS(3000));

//         ESP_LOGI(TAG, "TEST 2: DO1 OFF, DO2 ON");
//         ESP_ERROR_CHECK(board_tca_write_pin(1, false));
//         ESP_ERROR_CHECK(board_tca_write_pin(0, true));
//         vTaskDelay(pdMS_TO_TICKS(3000));

//         ESP_LOGI(TAG, "TEST 3: DO1 ON, DO2 ON");
//         ESP_ERROR_CHECK(board_tca_write_pin(1, true));
//         ESP_ERROR_CHECK(board_tca_write_pin(0, true));
//         vTaskDelay(pdMS_TO_TICKS(3000));

//         ESP_LOGI(TAG, "TEST 4: DO1 OFF, DO2 OFF");
//         ESP_ERROR_CHECK(board_tca_write_pin(1, false));
//         ESP_ERROR_CHECK(board_tca_write_pin(0, false));
//         vTaskDelay(pdMS_TO_TICKS(3000));
//     }
// }

// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "board.h"

// static const char *TAG = "main";

// #define TCA_PIN(port, bit)   ((port) * 8 + (bit))

// #define TCA_INPUT3           TCA_PIN(1, 5)   // P15
// #define TCA_INPUT2           TCA_PIN(1, 6)   // P16
// #define TCA_INPUT1           TCA_PIN(1, 7)   // P17

// void app_main(void)
// {
//     bool in1 = false;
//     bool in2 = false;
//     bool in3 = false;

//     ESP_ERROR_CHECK(board_init());

//     ESP_ERROR_CHECK(board_tca_set_pin_mode(TCA_INPUT1, true));
//     ESP_ERROR_CHECK(board_tca_set_pin_mode(TCA_INPUT2, true));
//     ESP_ERROR_CHECK(board_tca_set_pin_mode(TCA_INPUT3, true));

//     while (1) {
//         board_tca_read_pin(TCA_INPUT1, &in1);
//         board_tca_read_pin(TCA_INPUT2, &in2);
//         board_tca_read_pin(TCA_INPUT3, &in3);

//         ESP_LOGI(TAG, "IN1=%d IN2=%d IN3=%d",
//                  in1 ? 1 : 0,
//                  in2 ? 1 : 0,
//                  in3 ? 1 : 0);

//         vTaskDelay(pdMS_TO_TICKS(500));
//     }
// }

//Test Power
// #include "esp_err.h"
// #include "esp_log.h"

// #include "power_monitor.h"

// static const char *TAG = "app_main";

// void app_main(void)
// {
//     ESP_ERROR_CHECK(power_monitor_init());
//     ESP_ERROR_CHECK(power_monitor_start());
//     ESP_LOGI(TAG, "power_monitor component started");
// }

// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "tm1638.h"
// #include "app_config.h"

// void app_main(void)
// {
//     tm1638_t dev;
//     tm1638_init(&dev, APP_TM1638_STB_GPIO, APP_TM1638_CLK_GPIO, APP_TM1638_DIO_GPIO, 7);

//     while (1) {
       
//         tm1638_set_led(&dev, 6, false);
//         vTaskDelay(pdMS_TO_TICKS(1000));

//         printf("LED8 RED\n");
//         tm1638_set_led(&dev, 6, true);
//         vTaskDelay(pdMS_TO_TICKS(1000));



//     }
// }