#include "net_manager.h"
#include "modem_service.h"
#include "app_config.h"
#include "app_events.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "provision_service.h"
#include "runtime_config.h"
#include "tm1638_wifi_ui.h"
#include "wifi_service.h"
#include "ethernet_service.h"
#include <string.h>

static const char *TAG = "net_manager";

#ifndef APP_NET_MANAGER_TASK_STACK
#define APP_NET_MANAGER_TASK_STACK 4096
#endif

#ifndef APP_NET_MANAGER_TASK_PRIO
#define APP_NET_MANAGER_TASK_PRIO 8
#endif

#ifndef APP_WIFI_SETTLE_BEFORE_MQTT_MS
#define APP_WIFI_SETTLE_BEFORE_MQTT_MS 3000
#endif

#ifndef APP_WIFI_RETRY_DELAY_MS
#define APP_WIFI_RETRY_DELAY_MS 5000
#endif

#ifndef APP_WIFI_LOST_TO_4G_MS
#define APP_WIFI_LOST_TO_4G_MS 15000
#endif

#ifndef APP_WIFI_RECHECK_FROM_4G_MS
#define APP_WIFI_RECHECK_FROM_4G_MS (5 * 60 * 1000)         // Sau 5p kiem tra lại Ket noi Wifi
#endif

#ifndef APP_WIFI_STABLE_BEFORE_BACK_MS
#define APP_WIFI_STABLE_BEFORE_BACK_MS 15000
#endif

// typedef enum {
//     NETM_MODE_IDLE = 0,
//     NETM_MODE_PROVISION,
//     NETM_MODE_WIFI_CONNECTING,
//     NETM_MODE_WIFI_ONLINE,
//     NETM_MODE_PPP_CONNECTING,
//     NETM_MODE_PPP_ONLINE,
// } netm_mode_t;

typedef enum {
    NETM_MODE_IDLE = 0,
    NETM_MODE_PROVISION,
    NETM_MODE_ETH_CONNECTING,
    NETM_MODE_ETH_ONLINE,
    NETM_MODE_WIFI_CONNECTING,
    NETM_MODE_WIFI_ONLINE,
    NETM_MODE_PPP_CONNECTING,
    NETM_MODE_PPP_ONLINE,
} netm_mode_t;

static TaskHandle_t s_task;
static TaskHandle_t s_provision_task;
static bool s_started;
static bool s_net_ready;
static bool s_mqtt_ready;
static bool s_mqtt_start_pending;
static bool s_wifi_config_ready;
static TickType_t s_wifi_retry_due_tick;
static TickType_t s_wifi_settle_deadline_tick;
static runtime_config_t s_runtime_cfg;
static netm_mode_t s_mode = NETM_MODE_IDLE;
static app_net_type_t s_active_uplink = APP_NET_NONE;

static bool s_wifi_has_ip;
static bool s_ppp_has_ip;
static TickType_t s_wifi_lost_tick;
static TickType_t s_wifi_retry_from_4g_tick;
static TickType_t s_wifi_online_since_tick;

static TickType_t s_eth_wait_deadline_tick;
static bool s_wifi_start_after_eth_wait;

static bool s_eth_has_ip;

static bool wifi_stable_for_fallback(TickType_t now);
static void transition_mode(netm_mode_t new_mode, const char *reason);
static TickType_t s_mqtt_retry_due_tick;

static void start_eth_connect(void)
{
#if APP_ETH_ENABLE
    ESP_LOGI(TAG, "starting Ethernet/W5500");

    esp_err_t err = ethernet_service_start();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        transition_mode(NETM_MODE_ETH_CONNECTING, "start_eth_connect");
    } else {
        ESP_LOGW(TAG, "ethernet_service_start failed: %s", esp_err_to_name(err));
    }
#endif
}

static const char *mode_name(netm_mode_t mode)
{
    switch (mode) {
    case NETM_MODE_IDLE: return "IDLE";
    case NETM_MODE_PROVISION: return "PROVISION";
    case NETM_MODE_ETH_CONNECTING: return "ETH_CONNECTING";
    case NETM_MODE_ETH_ONLINE: return "ETH_ONLINE";
    case NETM_MODE_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case NETM_MODE_WIFI_ONLINE: return "WIFI_ONLINE";
    case NETM_MODE_PPP_CONNECTING: return "PPP_CONNECTING";
    case NETM_MODE_PPP_ONLINE: return "PPP_ONLINE";
    default: return "UNKNOWN";
    }
}

static void transition_mode(netm_mode_t new_mode, const char *reason)
{
    if (s_mode == new_mode) {
        return;
    }
    ESP_LOGI(TAG, "state %s -> %s (%s)",
             mode_name(s_mode), mode_name(new_mode), reason ? reason : "-");
    s_mode = new_mode;
}

static bool load_runtime_cfg(void)
{
    memset(&s_runtime_cfg, 0, sizeof(s_runtime_cfg));
    if (runtime_config_load(&s_runtime_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "runtime config load failed");
        return false;
    }

    if (s_runtime_cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi ssid empty");
        return false;
    }

    return true;
}

// static void reset_runtime_state(void)
// {
//     s_net_ready = false;
//     s_mqtt_ready = false;
//     s_mqtt_start_pending = false;
//     s_wifi_retry_due_tick = 0;
//     s_wifi_settle_deadline_tick = 0;
//     s_active_uplink = APP_NET_NONE;

//     s_wifi_has_ip = false;
//     s_ppp_has_ip = false;
//     s_wifi_lost_tick = 0;
//     s_wifi_retry_from_4g_tick = 0;
//     s_wifi_online_since_tick = 0;

//     s_eth_has_ip = false;
// }

static void reset_runtime_state(void)
{
    s_net_ready = false;
    s_mqtt_ready = false;
    s_mqtt_start_pending = false;
    s_wifi_retry_due_tick = 0;
    s_wifi_settle_deadline_tick = 0;
    s_mqtt_retry_due_tick = 0;
    s_active_uplink = APP_NET_NONE;

    s_wifi_has_ip = false;
    s_ppp_has_ip = false;
    s_wifi_lost_tick = 0;
    s_wifi_retry_from_4g_tick = 0;
    s_wifi_online_since_tick = 0;

    s_eth_has_ip = false;
}

static void stop_online_services(void)
{
    s_mqtt_start_pending = false;
    s_mqtt_ready = false;
    mqtt_service_stop();
    wifi_service_stop_sta();
}

static void enter_provision_mode_now(const char *reason)
{
    ESP_LOGW(TAG, "enter provision mode (%s)", reason ? reason : "-");
    tm1638_wifi_set_state(WIFI_LED_CONFIG_MODE);

    stop_online_services();
    wifi_service_set_reconnect_enabled(false);
    provision_service_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    if (provision_service_start() == ESP_OK) {
        s_active_uplink = APP_NET_AP_ONLY;
        transition_mode(NETM_MODE_PROVISION, reason ? reason : "provision");
    } else {
        s_active_uplink = APP_NET_NONE;
        transition_mode(NETM_MODE_IDLE, "provision_start_failed");
    }
}

static void enter_provision_task(void *arg)
{
    const char *reason = (const char *)arg;
    enter_provision_mode_now(reason ? reason : "button_request");
    s_provision_task = NULL;
    vTaskDelete(NULL);
}

static void request_enter_provision_mode(const char *reason)
{
    if (s_provision_task != NULL) {
        ESP_LOGW(TAG, "provision task already running, ignore (%s)", reason ? reason : "-");
        return;
    }

    BaseType_t ok = xTaskCreate(enter_provision_task,
                                "enter_prov",
                                4096,
                                (void *)reason,
                                (tskIDLE_PRIORITY + 2),
                                &s_provision_task);
    if (ok != pdPASS) {
        s_provision_task = NULL;
        ESP_LOGE(TAG, "failed to create provision task");
    }
}

static void start_wifi_connect(void)
{
    if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
        ESP_LOGI(TAG, "skip start Wi-Fi because Ethernet is online");
        return;
    }
    runtime_config_t cfg = {0};
    esp_err_t cfg_err = runtime_config_load(&cfg);

    tm1638_wifi_set_state(WIFI_LED_SCANNING);

    s_wifi_retry_due_tick = 0;
    s_wifi_settle_deadline_tick = 0;

    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_load failed: %s", esp_err_to_name(cfg_err));
        tm1638_wifi_set_state(WIFI_LED_OFF);
        s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_cfg_load_failed");
        return;
    }

    if (!cfg.wifi_enabled || cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi disabled or ssid empty");
        tm1638_wifi_set_state(WIFI_LED_OFF);
        s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_disabled");
        return;
    }

    esp_err_t err = wifi_service_start_sta(cfg.wifi_ssid, cfg.wifi_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_service_start_sta failed: %s", esp_err_to_name(err));
        tm1638_wifi_set_state(WIFI_LED_OFF);
        s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_start_failed");
        return;
    }

    transition_mode(NETM_MODE_WIFI_CONNECTING, "start_wifi_connect");
}

static void start_ppp_connect(void)
{
    ESP_LOGI(TAG, "starting PPP/4G");
    esp_err_t err = modem_service_start();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        transition_mode(NETM_MODE_PPP_CONNECTING, "start_ppp_connect");
    } else {
        ESP_LOGW(TAG, "modem_service_start failed: %s", esp_err_to_name(err));
    }
}

static void stop_ppp_connect(void)
{
    esp_err_t err = modem_service_stop_ppp();
    ESP_LOGI(TAG, "stop PPP: %s", esp_err_to_name(err));
}

static void maybe_start_mqtt(void)
{
    if (!s_net_ready) {
        return;
    }

    if (s_active_uplink != APP_NET_ETH &&
        s_active_uplink != APP_NET_WIFI &&
        s_active_uplink != APP_NET_PPP) {
        return;
    }

    if (s_mqtt_ready || mqtt_service_is_connected() || mqtt_service_is_started()) {
        return;
    }

    if (s_wifi_settle_deadline_tick != 0 && xTaskGetTickCount() < s_wifi_settle_deadline_tick) {
        return;
    }

    const char *uplink_name = "UNKNOWN";
    if (s_active_uplink == APP_NET_ETH) {
        uplink_name = "Ethernet";
    } else if (s_active_uplink == APP_NET_WIFI) {
        uplink_name = "Wi-Fi";
    } else if (s_active_uplink == APP_NET_PPP) {
        uplink_name = "4G";
    }

    ESP_LOGI(TAG, "starting mqtt over %s", uplink_name);

    // if (mqtt_service_start() == ESP_OK) {
    //     s_mqtt_start_pending = true;
    // } else {
    //     s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
    // }
    esp_err_t err = mqtt_service_start();
    if (err == ESP_OK) {
        s_mqtt_start_pending = true;
        s_mqtt_retry_due_tick = 0;
    } else {
        ESP_LOGW(TAG, "mqtt_service_start failed: %s", esp_err_to_name(err));
        s_mqtt_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
    }
}

// static void handle_timers(void)
// {
//     TickType_t now = xTaskGetTickCount();

//     if (s_wifi_retry_due_tick != 0 && now >= s_wifi_retry_due_tick) {
//         s_wifi_retry_due_tick = 0;

//         if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
//             ESP_LOGI(TAG, "skip Wi-Fi retry because Ethernet is online");
//         } else {
//             ESP_LOGW(TAG, "wifi retry timer expired -> reconnect");
//             start_wifi_connect();
//         }
//     }

//     if (s_wifi_settle_deadline_tick != 0 && now >= s_wifi_settle_deadline_tick) {
//         s_wifi_settle_deadline_tick = 0;
//         maybe_start_mqtt();
//     }
//     if (s_mode == NETM_MODE_PPP_ONLINE &&
//         s_wifi_retry_from_4g_tick != 0 &&
//         now >= s_wifi_retry_from_4g_tick) {
//         s_wifi_retry_from_4g_tick = now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

//         if (!wifi_service_is_connected()) {
//             ESP_LOGI(TAG, "PPP online -> periodic Wi-Fi retry");
//             start_wifi_connect();
//         }
//     }
// }

static void handle_timers(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_wifi_retry_due_tick != 0 && now >= s_wifi_retry_due_tick) {
        s_wifi_retry_due_tick = 0;

        if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
            ESP_LOGI(TAG, "skip Wi-Fi retry because Ethernet is online");
        } else {
            ESP_LOGW(TAG, "wifi retry timer expired -> reconnect");
            start_wifi_connect();
        }
    }

    if (s_wifi_settle_deadline_tick != 0 && now >= s_wifi_settle_deadline_tick) {
        s_wifi_settle_deadline_tick = 0;
        maybe_start_mqtt();
    }

    if (s_mqtt_retry_due_tick != 0 && now >= s_mqtt_retry_due_tick) {
        s_mqtt_retry_due_tick = 0;

        if (s_net_ready &&
            (s_active_uplink == APP_NET_ETH ||
             s_active_uplink == APP_NET_WIFI ||
             s_active_uplink == APP_NET_PPP)) {
            ESP_LOGI(TAG, "mqtt retry timer expired -> restart mqtt");
            maybe_start_mqtt();
        }
    }

    if (s_mode == NETM_MODE_PPP_ONLINE &&
        s_wifi_retry_from_4g_tick != 0 &&
        now >= s_wifi_retry_from_4g_tick) {
        s_wifi_retry_from_4g_tick = now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

        if (!wifi_service_is_connected()) {
            ESP_LOGI(TAG, "PPP online -> periodic Wi-Fi retry");
            start_wifi_connect();
        }
    }
}

static void app_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;

    switch (event_id) {
    case APP_EVENT_NET_UP: {
        app_net_status_t *st = (app_net_status_t *)data;
        if (!st) {
            break;
        }
        if (st->type == APP_NET_ETH) {
            ESP_LOGI(TAG, "Ethernet uplink online");

            s_eth_has_ip = true;
            s_wifi_start_after_eth_wait = false;
            s_eth_wait_deadline_tick = 0;

            if (s_active_uplink != APP_NET_ETH) {
                s_mqtt_ready = false;
                s_mqtt_start_pending = false;
                mqtt_service_stop();
            }

            s_net_ready = true;
            s_active_uplink = APP_NET_ETH;

            wifi_service_stop_sta();
            s_wifi_has_ip = false;
            s_wifi_lost_tick = 0;
            s_wifi_retry_due_tick = 0;
            s_wifi_online_since_tick = 0;

            if (s_mode == NETM_MODE_PPP_ONLINE || s_mode == NETM_MODE_PPP_CONNECTING) {
                stop_ppp_connect();
                s_ppp_has_ip = false;
            }

            s_wifi_settle_deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            transition_mode(NETM_MODE_ETH_ONLINE, "eth_net_up");
        }
        else if (st->type == APP_NET_WIFI) {
            ESP_LOGI(TAG, "Wi-Fi uplink online");

            if (s_eth_has_ip) {
                ESP_LOGI(TAG, "ignore Wi-Fi because Ethernet is online");
                break;
            }

            tm1638_wifi_set_state(WIFI_LED_READY);

            s_wifi_has_ip = true;
            s_net_ready = true;
            s_active_uplink = APP_NET_WIFI;

            s_wifi_retry_due_tick = 0;
            s_wifi_lost_tick = 0;
            s_wifi_online_since_tick = xTaskGetTickCount();
            s_wifi_settle_deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_SETTLE_BEFORE_MQTT_MS);

            if (s_mode == NETM_MODE_PPP_ONLINE || s_mode == NETM_MODE_PPP_CONNECTING) {
                ESP_LOGI(TAG, "Wi-Fi recovered -> switch back from 4G");
                stop_ppp_connect();
                s_ppp_has_ip = false;
            }

            transition_mode(NETM_MODE_WIFI_ONLINE, "wifi_net_up");
        }
        else if (st->type == APP_NET_PPP) {
            ESP_LOGI(TAG, "PPP uplink online");
            s_ppp_has_ip = true;

            if (!s_eth_has_ip && !s_wifi_has_ip) {
                s_net_ready = true;
                s_active_uplink = APP_NET_PPP;
                transition_mode(NETM_MODE_PPP_ONLINE, "ppp_net_up");
            }
        }

        break;
    }

    case APP_EVENT_NET_DOWN: {
        app_net_status_t *st = (app_net_status_t *)data;
        if (!st) {
            break;
        }

        if (st->type == APP_NET_ETH) {
            ESP_LOGW(TAG, "Ethernet uplink down");

            s_eth_has_ip = false;

            if (s_active_uplink == APP_NET_ETH) {
                s_net_ready = false;
                s_mqtt_ready = false;
                s_mqtt_start_pending = false;
                mqtt_service_stop();

                s_active_uplink = APP_NET_NONE;
                transition_mode(NETM_MODE_IDLE, "eth_net_down");

                if (s_wifi_config_ready) {
                    s_wifi_lost_tick = xTaskGetTickCount();
                    s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                    ESP_LOGI(TAG, "schedule Wi-Fi start after Ethernet down");
                } else {
                    request_enter_provision_mode("eth_down_wifi_config_missing");
                }
            }
        }
        else if (st->type == APP_NET_WIFI) {

             ESP_LOGW(TAG, "Wi-Fi uplink down");

            tm1638_wifi_set_state(WIFI_LED_OFF);

            s_wifi_has_ip = false;
            s_wifi_online_since_tick = 0;

            if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
                ESP_LOGI(TAG, "ignore Wi-Fi down because Ethernet is online");
                s_wifi_lost_tick = 0;
                s_wifi_retry_due_tick = 0;
                break;
            }
            if (s_active_uplink == APP_NET_WIFI) {
                s_net_ready = false;
                s_mqtt_ready = false;
                s_mqtt_start_pending = false;
                mqtt_service_stop();

                if (s_wifi_lost_tick == 0) {
                    s_wifi_lost_tick = xTaskGetTickCount();
                }

                transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_net_down");
                s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
            }
        }
        else if (st->type == APP_NET_PPP) {
            ESP_LOGW(TAG, "PPP uplink down");

            s_ppp_has_ip = false;

            if (s_active_uplink == APP_NET_PPP) {
                s_net_ready = false;
                s_mqtt_ready = false;
                s_mqtt_start_pending = false;
                mqtt_service_stop();

                s_active_uplink = APP_NET_NONE;
                transition_mode(NETM_MODE_IDLE, "ppp_net_down");
            }
        }

        break;
    }

    case APP_EVENT_MQTT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_ready = true;
        s_mqtt_start_pending = false;
        s_mqtt_retry_due_tick = 0;
        break;

    // case APP_EVENT_MQTT_DISCONNECTED:
    //     ESP_LOGW(TAG, "MQTT disconnected");

    //     s_mqtt_ready = false;
    //     s_mqtt_start_pending = false;

    //     if (s_net_ready &&
    //         (s_active_uplink == APP_NET_ETH ||
    //          s_active_uplink == APP_NET_WIFI ||
    //          s_active_uplink == APP_NET_PPP)) {
    //         s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
    //     }
    //     break;
    case APP_EVENT_MQTT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");

        s_mqtt_ready = false;
        s_mqtt_start_pending = false;

        /*
        * MQTT loi thi chi retry MQTT.
        * Khong duoc reconnect Wi-Fi o day, vi Wi-Fi van co IP binh thuong.
        */
        if (s_net_ready &&
            (s_active_uplink == APP_NET_ETH ||
            s_active_uplink == APP_NET_WIFI ||
            s_active_uplink == APP_NET_PPP)) {
            s_mqtt_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        }

        break;
    case APP_EVENT_WIFI_CONFIG_SAVED:
        ESP_LOGI(TAG, "Wi-Fi config saved -> reload and reconnect");

        s_wifi_config_ready = load_runtime_cfg();

        if (!s_eth_has_ip) {
            start_wifi_connect();
        } else {
            ESP_LOGI(TAG, "skip Wi-Fi reconnect because Ethernet is online");
        }
        break;

    case APP_EVENT_PROVISION_START:
        request_enter_provision_mode("button_request");
        break;

    default:
        break;
    }
}

static void net_manager_task(void *arg)
{
    (void)arg;

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        handle_timers();
        maybe_start_mqtt();

        if (!s_eth_has_ip &&
            s_active_uplink != APP_NET_ETH &&
            !s_wifi_has_ip &&
            !s_ppp_has_ip &&
            s_wifi_lost_tick != 0 &&
            now >= s_wifi_lost_tick + pdMS_TO_TICKS(APP_WIFI_LOST_TO_4G_MS)) {

            s_wifi_lost_tick = 0;
            ESP_LOGW(TAG, "Wi-Fi lost too long -> switch to 4G");
            start_ppp_connect();
        }

        if (s_mode == NETM_MODE_PPP_ONLINE && s_wifi_retry_from_4g_tick == 0) {
            s_wifi_retry_from_4g_tick = now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);
        }

        if (s_mode == NETM_MODE_PPP_ONLINE && wifi_stable_for_fallback(now)) {
            ESP_LOGI(TAG, "Wi-Fi stable -> switch back from 4G");

            s_net_ready = true;
            s_active_uplink = APP_NET_WIFI;
            transition_mode(NETM_MODE_WIFI_ONLINE, "wifi_fallback_from_4g");

            stop_ppp_connect();
            s_ppp_has_ip = false;
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t net_manager_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_wifi_config_ready = load_runtime_cfg();
    reset_runtime_state();
    transition_mode(NETM_MODE_IDLE, "init");

    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID, app_event_handler, NULL));
    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_started = true;

#if APP_ETH_ENABLE
    start_eth_connect();
#endif

    if (!s_wifi_config_ready) {
        request_enter_provision_mode("wifi_config_missing");
    } else {
        /*
         * Vẫn start Wi-Fi song song.
         * Nếu Ethernet lên thì sẽ tự ưu tiên Ethernet và stop Wi-Fi STA.
         */
        start_wifi_connect();
    }

    BaseType_t ok = xTaskCreate(net_manager_task,
                                "net_manager",
                                APP_NET_MANAGER_TASK_STACK,
                                NULL,
                                APP_NET_MANAGER_TASK_PRIO,
                                &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static bool wifi_stable_for_fallback(TickType_t now)
{
    return s_wifi_has_ip &&
           s_wifi_online_since_tick != 0 &&
           (now - s_wifi_online_since_tick) >= pdMS_TO_TICKS(APP_WIFI_STABLE_BEFORE_BACK_MS);
}