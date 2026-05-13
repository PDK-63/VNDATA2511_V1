#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/adc.h"

#define APP_PROJECT_NAME                 "Fw_Basevn2411_prod"
#define APP_FW_VERSION                   "1.0.0"

#define APP_DEVICE_ID                    "2511"
#define APP_TOPIC_PREFIX                 "VN2402"

/* Modem UART */
#define APP_MODEM_UART_PORT              UART_NUM_1
#define APP_MODEM_UART_TX                GPIO_NUM_17
#define APP_MODEM_UART_RX                GPIO_NUM_18
#define APP_MODEM_BAUDRATE               115200
#define APP_MODEM_APN                    "v-internet"
#define APP_DISABLE_PPP                  1

/* I2C / TCA9555 */
#define APP_I2C_MASTER_PORT              I2C_NUM_0
#define APP_I2C_MASTER_SDA_IO            GPIO_NUM_39
#define APP_I2C_MASTER_SCL_IO            GPIO_NUM_40
#define APP_I2C_MASTER_FREQ_HZ           400000

#define APP_TCA9555_ADDR                 0x20
#define APP_TCA9555_INT_GPIO             GPIO_NUM_6
#define APP_TCA_PIN_N_NET                10
#define APP_TCA_PIN_N_RESET              11
#define APP_TCA_PIN_N_STA                12
#define APP_TCA_PIN_N_PWR                13

#define APP_MODEM_PWRKEY_ASSERT_LEVEL    1
#define APP_MODEM_PWRKEY_IDLE_LEVEL      0
#define APP_MODEM_RESET_ASSERT_LEVEL     1
#define APP_MODEM_RESET_IDLE_LEVEL       0
#define APP_MODEM_PWRKEY_PULSE_MS        1200
#define APP_MODEM_RESET_PULSE_MS         250
#define APP_MODEM_BOOT_WAIT_MS           8000
#define APP_MODEM_AFTER_RESET_WAIT_MS    8000

/* Sensors / display */
#define APP_NTC1_ADC_CHANNEL              ADC1_CHANNEL_0
#define APP_NTC2_ADC_CHANNEL             ADC1_CHANNEL_1
#define APP_SHT30_I2C_PORT               I2C_NUM_1
#define APP_SHT30_SCL_GPIO               GPIO_NUM_42
#define APP_SHT30_SDA_GPIO               GPIO_NUM_41
#define APP_SHT30_I2C_ADDR               0x44
#define APP_SHT30_I2C_FREQ_HZ            100000
#define APP_TM1638_STB_GPIO              GPIO_NUM_46
#define APP_TM1638_CLK_GPIO              GPIO_NUM_47
#define APP_TM1638_DIO_GPIO              GPIO_NUM_48
#define APP_TM1638_BRIGHTNESS            3

#define APP_TEMP_ALARM_C                 35.0f
#define APP_SMS_ALERT_NUMBER             "0977106348"
#define APP_TEMP_HYSTERESIS_C            2.0f
#define APP_NTC_LOW_LIMIT_C              -50.0f
#define APP_NTC_HIGH_LIMIT_C             100.0f
#define APP_HUM_LOW_LIMIT_PCT            0.0f
#define APP_HUM_HIGH_LIMIT_PCT           100.0f
#define APP_SMS_COMMAND_POLL_MS          10000

/* MQTT */
#define APP_MQTT_URI                      "mqtt://giamsatnhietdo.vn:1883"                             // "mqtts://broker.hivemq.com:8883"
#define APP_MQTT_USERNAME                "mqtt"
#define APP_MQTT_PASSWORD                "giamsatnhietdo@123"
#define APP_MQTT_KEEPALIVE_SEC           30

#define APP_MQTT_TOPIC_STATUS_FMT        "%s/%s/statuss"
#define APP_MQTT_TOPIC_STATE_FMT         "%s/%s/states"
#define APP_MQTT_TOPIC_TELEMETRY_FMT     "%s/%s/cfg/sensor"
#define APP_MQTT_TOPIC_CMD_FMT           "%s/%s/cfg/cmd"
#define APP_MQTT_TOPIC_CMD_GET_RESPONE     "%s/%s/get/response"

#define APP_MQTT_TOPIC_SETTINGS_FMT         "%s/%s/cfg/setting"

#define APP_MQTT_BIRTH_PAYLOAD           "{\"device_id\":\"" APP_DEVICE_ID "\",\"deviceState\":\"online\"}"
#define APP_MQTT_LWT_PAYLOAD             "{\"device_id\":\"" APP_DEVICE_ID "\",\"deviceState\":\"offline\"}"

#define APP_OTA_URL_DEFAULT              ""

/* Wi-Fi provisioning / AP */
#define APP_WIFI_AP_SSID                 "VN2511_SETUP"
#define APP_WIFI_AP_PASS                 "12345678"
#define APP_WIFI_AP_CHANNEL              1
#define APP_WIFI_AP_MAX_CONN             4
#define APP_WIFI_AP_IP                   "192.168.4.1"
#define APP_WIFI_AP_GATEWAY              "192.168.4.1"
#define APP_WIFI_AP_NETMASK              "255.255.255.0"

#define APP_WIFI_STA_MAX_RETRY           10
#define APP_WIFI_FAILOVER_DELAY_MS       30000
#define APP_WIFI_RETRY_DELAY_MS          5000
#define APP_WIFI_RETRY_FROM_PPP_MS       120000
#define APP_WIFI_STABLE_BEFORE_SWITCH_MS 15000

#define APP_DEFAULT_PUB_MS               60000      // Time pub 1 message lên MQTT
#define APP_MIN_PUB_MS                   1000
#define APP_MAX_PUB_MS                   3600000
#define APP_HEALTH_LOG_MS                300000
#define APP_IP_WAIT_MS                   90000
#define APP_PPP_RECOVERY_DELAY_MS        15000
#define APP_MQTT_RECOVERY_DELAY_MS       10000
#define APP_MODEM_SYNC_RETRIES           8
#define APP_MODEM_MAX_RECOVERIES         5
#define APP_MQTT_MAX_RECOVERIES          8
#define APP_MAX_MODEM_POWERCYCLES        3
#define APP_FACTORY_RESET_DELAY_MS       1000
#define APP_PROVISION_BUTTON_GPIO        GPIO_NUM_3
#define APP_PROVISION_BUTTON_ACTIVE_LEVEL 0
#define APP_PROVISION_BUTTON_HOLD_MS     2000
#define APP_PROVISION_BUTTON_POLL_MS     100

// Define Pin Ethernet use IC W5500
#define APP_ETH_ENABLE                    1
#define APP_ETH_WAIT_BEFORE_WIFI_MS       5000

#define APP_ETH_SPI_HOST                  SPI2_HOST
#define APP_ETH_SPI_MOSI_GPIO             GPIO_NUM_38
#define APP_ETH_SPI_MISO_GPIO             GPIO_NUM_45
#define APP_ETH_SPI_SCLK_GPIO             GPIO_NUM_21
#define APP_ETH_SPI_CS_GPIO               GPIO_NUM_9
#define APP_ETH_SPI_INT_GPIO              GPIO_NUM_8
#define APP_ETH_SPI_RST_GPIO              GPIO_NUM_7

#define APP_ETH_SPI_CLOCK_MHZ             4

/*Bien su dung voi SD Card */
#define APP_SDMMC_ENABLE                  1

#define APP_SD_DET_GPIO                   GPIO_NUM_16

#define APP_SD_DET_ACTIVE_LEVEL           0

#define APP_SDMMC_D0_GPIO                 GPIO_NUM_10
#define APP_SDMMC_D1_GPIO                 GPIO_NUM_11
#define APP_SDMMC_D2_GPIO                 GPIO_NUM_12
#define APP_SDMMC_D3_GPIO                 GPIO_NUM_13
#define APP_SDMMC_CMD_GPIO                GPIO_NUM_14
#define APP_SDMMC_CLK_GPIO                GPIO_NUM_15

#define APP_SDMMC_BUS_WIDTH               1

#define APP_SD_LOG_INTERVAL_MS            (5 * 60 * 1000)

