#include "ethernet_service.h"

#include "app_config.h"
#include "app_events.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_mac_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_mac.h"
static const char *TAG = "ethernet_service";

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static spi_device_handle_t s_spi_handle = NULL;

static bool s_inited = false;
static bool s_started = false;
static bool s_link_up = false;
static bool s_has_ip = false;

static void post_net_status(app_net_type_t type, bool up)
{
    app_net_status_t st = {
        .type = type,
        .has_ip = up,
    };

    esp_event_post(APP_EVENTS,
                   up ? APP_EVENT_NET_UP : APP_EVENT_NET_DOWN,
                   &st,
                   sizeof(st),
                   portMAX_DELAY);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "W5500 link up");
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 link down");
        s_link_up = false;
        s_has_ip = false;
        post_net_status(APP_NET_ETH, false);
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        s_link_up = false;
        s_has_ip = false;
        break;

    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_ETH_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (!event) {
        return;
    }

    s_has_ip = true;

    ESP_LOGI(TAG, "W5500 got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    post_net_status(APP_NET_ETH, true);
}

esp_err_t ethernet_service_init(void)
{
#if !APP_ETH_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err;

    spi_bus_config_t buscfg = {
        .mosi_io_num = APP_ETH_SPI_MOSI_GPIO,
        .miso_io_num = APP_ETH_SPI_MISO_GPIO,
        .sclk_io_num = APP_ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };

    err = spi_bus_initialize(APP_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = APP_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 16,
        .spics_io_num = APP_ETH_SPI_CS_GPIO,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(APP_ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = APP_ETH_SPI_INT_GPIO;   // nếu không nối INT thì để -1

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = APP_ETH_SPI_RST_GPIO;   // nếu không nối reset thì để -1

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
    * W5500 không tự có MAC address.
    * Nếu không set, log sẽ ra 00:00:00:00:00:00 và DHCP có thể không cấp IP.
    */
    uint8_t eth_mac[6] = {0};

    err = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac(ESP_MAC_ETH) failed: %s, fallback to default efuse mac",
                esp_err_to_name(err));

        err = esp_efuse_mac_get_default(eth_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s", esp_err_to_name(err));
            return err;
        }

        /*
        * Tạo locally administered unicast MAC cho Ethernet.
        */
        eth_mac[0] = (eth_mac[0] | 0x02) & 0xFE;
        eth_mac[5] ^= 0x01;
    }

    ESP_LOGI(TAG, "set W5500 MAC: %02X:%02X:%02X:%02X:%02X:%02X",
            eth_mac[0], eth_mac[1], eth_mac[2],
            eth_mac[3], eth_mac[4], eth_mac[5]);

    err = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set W5500 MAC failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new ETH failed");
        return ESP_FAIL;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    s_inited = true;
    ESP_LOGI(TAG, "ethernet_service init done");
    return ESP_OK;
}

esp_err_t ethernet_service_start(void)
{
#if !APP_ETH_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (!s_inited) {
        esp_err_t err = ethernet_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t ethernet_service_stop(void)
{
    if (!s_started || !s_eth_handle) {
        return ESP_OK;
    }

    esp_err_t err = esp_eth_stop(s_eth_handle);
    if (err == ESP_OK) {
        s_started = false;
        s_link_up = false;
        s_has_ip = false;
    }
    return err;
}

bool ethernet_service_is_started(void)
{
    return s_started;
}

bool ethernet_service_is_link_up(void)
{
    return s_link_up;
}

bool ethernet_service_has_ip(void)
{
    return s_has_ip;
}