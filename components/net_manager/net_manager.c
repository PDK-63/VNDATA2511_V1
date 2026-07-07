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
#include "tm1638_server_ui.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_attr.h"

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
#define APP_WIFI_RECHECK_FROM_4G_MS (3 * 60 * 1000)         // Sau 5p kiem tra lại Ket noi Wifi
#endif

#ifndef APP_WIFI_STABLE_BEFORE_BACK_MS
#define APP_WIFI_STABLE_BEFORE_BACK_MS 15000
#endif

#ifndef APP_MQTT_RESTART_AFTER_MS
#define APP_MQTT_RESTART_AFTER_MS       (3 * 60 * 1000)
#endif

#ifndef APP_UPLINK_RECOVER_AFTER_MS
#define APP_UPLINK_RECOVER_AFTER_MS     (5 * 60 * 1000)
#endif

#ifndef APP_MODEM_RECOVER_AFTER_MS
#define APP_MODEM_RECOVER_AFTER_MS      (10 * 60 * 1000)
#endif

#ifndef APP_FORCE_REBOOT_NO_SERVER_MS
#define APP_FORCE_REBOOT_NO_SERVER_MS   (15 * 60 * 1000)
#endif

#ifndef APP_PPP_SETTLE_BEFORE_MQTT_MS
#define APP_PPP_SETTLE_BEFORE_MQTT_MS 10000
#endif

#ifndef APP_PPP_MQTT_RECOVER_GUARD_MS
#define APP_PPP_MQTT_RECOVER_GUARD_MS 15000
#endif

#define APP_SERVER_LOST_RESET_MS      (5UL * 60UL * 1000UL)
#define APP_SERVER_LOST_LOG_MS        60000UL

#define NETM_RESET_MAGIC                    0x4E45544DUL
#define NETM_RESET_REASON_SERVER_LOST       1

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

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t lost_ms;
    uint32_t active_uplink;
    uint32_t mode;
} netm_reset_marker_t;

static RTC_NOINIT_ATTR netm_reset_marker_t s_netm_reset_marker;

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

static TickType_t s_wifi_lost_tick;
static TickType_t s_wifi_retry_from_4g_tick;
static TickType_t s_wifi_online_since_tick;
static TickType_t s_wifi_connect_start_tick;

static TickType_t s_eth_wait_deadline_tick;
static bool s_wifi_start_after_eth_wait;

static bool s_eth_has_ip;
static bool s_wifi_has_ip;
static bool s_ppp_has_ip;

static void transition_mode(netm_mode_t new_mode, const char *reason);

static TickType_t s_mqtt_retry_due_tick;
static TickType_t s_ppp_retry_due_tick;
static app_net_type_t s_mqtt_uplink = APP_NET_NONE;

static TickType_t s_mqtt_down_since_tick;
static TickType_t s_last_mqtt_recover_tick;
static TickType_t s_last_uplink_recover_tick;
static TickType_t s_last_modem_recover_tick;

static TickType_t s_mqtt_start_tick;

static TickType_t s_ppp_mqtt_recover_until_tick = 0;

static TickType_t s_server_lost_since_tick = 0;
static TickType_t s_server_lost_last_log_tick = 0;

static bool s_server_lost_reset_pending = false;
static TickType_t s_server_lost_reset_pending_tick = 0;

static void netm_reset_mqtt_state(const char *reason)
{
    ESP_LOGW(TAG, "reset MQTT state: %s", reason ? reason : "-");

    s_mqtt_ready = false;
    s_mqtt_start_pending = false;
    s_mqtt_retry_due_tick = 0;
    s_mqtt_start_tick = 0;
    s_mqtt_uplink = APP_NET_NONE;

    if (mqtt_service_is_started() || mqtt_service_is_connected()) {
        mqtt_service_stop();
    }
}

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

bool net_manager_is_ppp_mqtt_recovering(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_active_uplink != APP_NET_PPP) {
        return false;
    }

    if (!s_ppp_has_ip && !modem_service_is_ip_ready()) {
        return false;
    }

    if (mqtt_service_is_connected()) {
        return false;
    }

    return (s_ppp_mqtt_recover_until_tick != 0 &&
            now < s_ppp_mqtt_recover_until_tick);
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

static void reset_runtime_state(void)
{
    s_net_ready = false;
    s_mqtt_ready = false;
    s_mqtt_start_pending = false;
    s_wifi_retry_due_tick = 0;
    s_wifi_settle_deadline_tick = 0;
    s_mqtt_retry_due_tick = 0;
     s_ppp_retry_due_tick = 0;
    s_active_uplink = APP_NET_NONE;

    s_wifi_has_ip = false;
    s_ppp_has_ip = false;
    s_wifi_lost_tick = 0;
    s_wifi_retry_from_4g_tick = 0;
    s_wifi_online_since_tick = 0;

    s_eth_has_ip = false;

     s_ppp_mqtt_recover_until_tick = 0;
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

    /*
     * Dat PROVISION som de cac event Wi-Fi down / retry timer
     * khong keo thiet bi quay lai STA.
     */
    s_active_uplink = APP_NET_AP_ONLY;
    transition_mode(NETM_MODE_PROVISION, reason ? reason : "provision");

    s_net_ready = false;
    s_mqtt_ready = false;
    s_mqtt_start_pending = false;

    s_wifi_retry_due_tick = 0;
    s_wifi_settle_deadline_tick = 0;

    /*
     * Neu ban co cac bien nay trong ban moi thi clear luon:
     */
    // s_wifi_retry_from_4g_tick = 0;
    // s_wifi_connect_start_tick = 0;
    // s_wifi_online_since_tick = 0;
    // s_wifi_lost_tick = 0;
    // s_mqtt_retry_due_tick = 0;

    tm1638_wifi_set_state(WIFI_LED_CONFIG_MODE);

    wifi_service_set_reconnect_enabled(false);
    mqtt_service_stop();
    wifi_service_stop_sta();

    provision_service_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    if (provision_service_restart() == ESP_OK) {
        s_active_uplink = APP_NET_AP_ONLY;
        transition_mode(NETM_MODE_PROVISION, reason ? reason : "provision_ok");
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
    wifi_service_set_reconnect_enabled(true);
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

static void start_wifi_probe_from_4g(void)
{
    runtime_config_t cfg = {0};
    esp_err_t cfg_err = runtime_config_load(&cfg);

    ESP_LOGI(TAG, "start WiFi probe from 4G");

    if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
        ESP_LOGI(TAG, "skip WiFi probe because Ethernet is online");
        return;
    }

    tm1638_wifi_set_state(WIFI_LED_SCANNING);

    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "WiFi probe: runtime_config_load failed: %s",
                 esp_err_to_name(cfg_err));
        tm1638_wifi_set_state(WIFI_LED_OFF);
        return;
    }

    if (!cfg.wifi_enabled || cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi probe: wifi disabled or ssid empty");
        tm1638_wifi_set_state(WIFI_LED_OFF);
        return;
    }

    wifi_service_set_reconnect_enabled(true);

    esp_err_t err = wifi_service_start_sta(cfg.wifi_ssid, cfg.wifi_pass);

    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG,
                "WiFi probe from 4G start failed: %s",
                esp_err_to_name(err));
        tm1638_wifi_set_state(WIFI_LED_OFF);
        return;
    }

    if (err == ESP_ERR_WIFI_CONN) {
        ESP_LOGI(TAG, "WiFi probe already connecting");
    }

    /*
     * Khong transition_mode(NETM_MODE_WIFI_CONNECTING) o day.
     * PPP/4G van la uplink chinh trong luc probe WiFi.
     */
}

static void start_ppp_connect(void)
{
    /*
     * Không start PPP nếu modem đang được dùng cho SMS/call.
     */
    if (modem_service_is_cs_session_active()) {
        ESP_LOGW(TAG, "skip PPP start because modem CS session active");
        return;
    }

    /*
     * Ethernet ưu tiên cao nhất.
     */
    if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
        ESP_LOGI(TAG, "skip PPP start because Ethernet is online");
        return;
    }

    /*
     * WiFi ưu tiên cao hơn 4G.
     */
    if (s_wifi_has_ip || s_active_uplink == APP_NET_WIFI) {
        ESP_LOGI(TAG, "skip PPP start because WiFi is online");
        return;
    }

    /*
     * Chặn start PPP chồng.
     * Phải check cả flag, active_uplink và mode vì sau nhiều lần chuyển mạng,
     * một trong các state có thể cập nhật chậm/lệch.
     */
    bool ppp_ip_real = s_ppp_has_ip || modem_service_is_ip_ready();

    if (ppp_ip_real ||
        s_mode == NETM_MODE_PPP_CONNECTING) {

        ESP_LOGI(TAG,
                "skip PPP start because PPP already active/connecting ppp_ip=%d active=%d mode=%d",
                ppp_ip_real ? 1 : 0,
                (int)s_active_uplink,
                (int)s_mode);
        return;
    }

    /*
    * Nếu state cũ nói PPP_ONLINE/active=PPP nhưng thực tế không có IP,
    * cho phép sửa state để start lại PPP.
    */
    if ((s_active_uplink == APP_NET_PPP || s_mode == NETM_MODE_PPP_ONLINE) &&
        !ppp_ip_real) {

        ESP_LOGW(TAG,
                "PPP state stale active=%d mode=%d but no real IP -> force reconnect",
                (int)s_active_uplink,
                (int)s_mode);

        s_ppp_has_ip = false;
        s_net_ready = false;
        s_active_uplink = APP_NET_NONE;
        transition_mode(NETM_MODE_IDLE, "ppp_state_stale");
    }

    ESP_LOGI(TAG, "starting PPP/4G");

    transition_mode(NETM_MODE_PPP_CONNECTING, "start_ppp_connect");

    esp_err_t err = modem_service_start();

    /*
     * ESP_ERR_INVALID_STATE thường nghĩa là modem service đã started/đang chạy.
     * Không coi là lỗi nặng, nhưng vẫn không nên schedule retry liên tục.
     */
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_ppp_retry_due_tick = 0;
        return;
    }

    ESP_LOGW(TAG, "modem_service_start failed: %s", esp_err_to_name(err));

    s_ppp_has_ip = false;
    s_net_ready = false;
    s_active_uplink = APP_NET_NONE;
    transition_mode(NETM_MODE_IDLE, "ppp_start_failed");

    /*
    * PPP fail, nhưng nếu còn cấu hình WiFi thì vẫn phải thử WiFi lại.
    * Case quan trọng: không có SIM, WiFi bị tắt rồi bật lại.
    */
    if (s_wifi_config_ready && !s_eth_has_ip && !s_wifi_has_ip) {
        s_wifi_retry_due_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);

        ESP_LOGI(TAG, "PPP failed/no uplink -> schedule WiFi retry");
    }

    /*
     * Retry chậm để tránh spam modem khi SIM hết data/sóng yếu.
     */
    s_ppp_retry_due_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(60000);
}

static void stop_ppp_connect(void)
{
    esp_err_t err = modem_service_stop_ppp();
    ESP_LOGI(TAG, "stop PPP: %s", esp_err_to_name(err));
}

static void maybe_start_mqtt(void)
{
    TickType_t now = xTaskGetTickCount();

    if (!s_net_ready) {
        return;
    }

    if (s_active_uplink != APP_NET_ETH &&
        s_active_uplink != APP_NET_WIFI &&
        s_active_uplink != APP_NET_PPP) {
        return;
    }

    /*
     * Neu dang hen retry MQTT thi khong duoc start som.
     */
    if (s_mqtt_retry_due_tick != 0 && now < s_mqtt_retry_due_tick) {
        return;
    }

    /*
     * Da toi han retry thi clear de cho phep start lai.
     */
    if (s_mqtt_retry_due_tick != 0 && now >= s_mqtt_retry_due_tick) {
        s_mqtt_retry_due_tick = 0;
    }

    /*
     * Dang cho settle sau khi doi uplink thi khong start MQTT som.
     */
    if (s_wifi_settle_deadline_tick != 0 &&
        now < s_wifi_settle_deadline_tick) {
        return;
    }

    /*
     * Da qua settle thi clear deadline.
     */
    if (s_wifi_settle_deadline_tick != 0 &&
        now >= s_wifi_settle_deadline_tick) {
        s_wifi_settle_deadline_tick = 0;
    }

    /*
     * Dang PPP ma modem dang CS session hoac PPP chua co IP thi khong start MQTT.
     */
    if (s_active_uplink == APP_NET_PPP &&
        (modem_service_is_cs_session_active() ||
         !modem_service_is_ip_ready())) {
        if (s_mqtt_retry_due_tick == 0) {
            s_mqtt_retry_due_tick = now + pdMS_TO_TICKS(5000);
        }
        return;
    }

    /*
     * Da connected thi kiem tra co dung uplink hien tai khong.
     */
    if (s_mqtt_ready || mqtt_service_is_connected()) {
        if (s_mqtt_uplink == s_active_uplink) {
            return;
        }

        ESP_LOGW(TAG,
                 "MQTT connected on old uplink=%d, active=%d -> stop old MQTT",
                 (int)s_mqtt_uplink,
                 (int)s_active_uplink);

        s_mqtt_ready = false;
        s_mqtt_start_pending = false;
        s_mqtt_uplink = APP_NET_NONE;
        s_mqtt_start_tick = 0;

        mqtt_service_stop();

        /*
         * Doi 1s roi start lai tren uplink moi.
         */
        s_mqtt_retry_due_tick = now + pdMS_TO_TICKS(1000);
        return;
    }

    /*
     * Neu dang start/connecting thi cho no co thoi gian connect.
     * Khong duoc stop qua som.
     */
    if (s_mqtt_start_pending || mqtt_service_is_started()) {
        uint32_t connect_timeout_ms = 20000UL;

        if (s_active_uplink == APP_NET_PPP) {
            connect_timeout_ms = 45000UL;
        }

        if (s_mqtt_start_tick != 0 &&
            pdTICKS_TO_MS(now - s_mqtt_start_tick) < connect_timeout_ms) {
            return;
        }

        if (s_mqtt_start_tick == 0) {
            ESP_LOGW(TAG,
                    "MQTT started with no start tick on uplink=%d -> stop stale client",
                    (int)s_active_uplink);
        } else {
            ESP_LOGW(TAG,
                    "MQTT start timeout on uplink=%d -> stop and retry",
                    (int)s_active_uplink);
        }

        s_mqtt_ready = false;
        s_mqtt_start_pending = false;
        s_mqtt_uplink = APP_NET_NONE;
        s_mqtt_start_tick = 0;

        mqtt_service_stop();

        uint32_t retry_ms = APP_WIFI_RETRY_DELAY_MS;
        if (s_active_uplink == APP_NET_PPP) {
            retry_ms = 20000UL;
        }

        s_mqtt_retry_due_tick = now + pdMS_TO_TICKS(retry_ms);
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

    ESP_LOGI(TAG, "starting MQTT over %s", uplink_name);

    s_mqtt_ready = false;
    s_mqtt_start_pending = true;
    s_mqtt_retry_due_tick = 0;
    s_mqtt_start_tick = now;
    s_mqtt_uplink = s_active_uplink;

    esp_err_t err = mqtt_service_start();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mqtt_service_start failed: %s", esp_err_to_name(err));

        s_mqtt_ready = false;
        s_mqtt_start_pending = false;
        s_mqtt_uplink = APP_NET_NONE;
        s_mqtt_start_tick = 0;

        uint32_t retry_ms = APP_WIFI_RETRY_DELAY_MS;
        if (s_active_uplink == APP_NET_PPP) {
            retry_ms = 20000UL;
        }

        s_mqtt_retry_due_tick = now + pdMS_TO_TICKS(retry_ms);
    }
}

static bool netm_restart_should_defer(void)
{
    /*
     * Đang thao tác modem SMS/call/poll thì không reset ngay.
     * Tránh reset giữa lúc đang gửi cảnh báo.
     */
    if (modem_service_is_cs_session_active()) {
        ESP_LOGW(TAG, "restart deferred: modem CS session active");
        return true;
    }

    /*
     * Nếu đang WiFi connecting hoặc PPP connecting thì có thể chờ thêm.
     * Tránh reset đúng lúc đang thử reconnect.
     */
    if (s_mode == NETM_MODE_WIFI_CONNECTING ||
        s_mode == NETM_MODE_PPP_CONNECTING) {
        ESP_LOGW(TAG,
                 "restart deferred: network connecting mode=%d",
                 (int)s_mode);
        return true;
    }

    return false;
}

static void netm_mark_restart_reason(uint32_t reason, uint32_t lost_ms)
{
    s_netm_reset_marker.magic = NETM_RESET_MAGIC;
    s_netm_reset_marker.reason = reason;
    s_netm_reset_marker.lost_ms = lost_ms;
    s_netm_reset_marker.active_uplink = (uint32_t)s_active_uplink;
    s_netm_reset_marker.mode = (uint32_t)s_mode;
}

static void netm_server_lost_watchdog(TickType_t now)
{
    bool mqtt_online = mqtt_service_is_connected();

    /*
     * Nếu server đã connected lại thì clear toàn bộ watchdog.
     */
    if (mqtt_online) {
        s_server_lost_since_tick = 0;
        s_server_lost_last_log_tick = 0;
        s_server_lost_reset_pending = false;
        s_server_lost_reset_pending_tick = 0;
        return;
    }

    /*
     * Nếu đang có pending reset từ trước, kiểm tra xem đã hết bận chưa.
     */
    if (s_server_lost_reset_pending) {
        if (!netm_restart_should_defer()) {
            uint32_t pending_ms =
                (uint32_t)((now - s_server_lost_reset_pending_tick) * portTICK_PERIOD_MS);

            ESP_LOGE(TAG,
                     "server lost pending reset now execute, pending_ms=%lu",
                     (unsigned long)pending_ms);

            netm_mark_restart_reason(NETM_RESET_REASON_SERVER_LOST, pending_ms);

            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        return;
    }

    /*
     * Nếu thiết bị chưa có bất kỳ cấu hình/kênh mạng nào,
     * reset cũng không giúp kết nối server.
     *
     * Nhưng nếu đã có WiFi config thì vẫn cho watchdog chạy,
     * vì thiết bị phải tự bắt lại WiFi/server.
     */
    if (!s_wifi_config_ready &&
        !s_eth_has_ip &&
        !s_wifi_has_ip &&
        !s_ppp_has_ip) {

        s_server_lost_since_tick = 0;
        s_server_lost_last_log_tick = 0;
        return;
    }

    /*
     * Bắt đầu tính thời gian mất server.
     */
    if (s_server_lost_since_tick == 0) {
        s_server_lost_since_tick = now;
        s_server_lost_last_log_tick = now;

        ESP_LOGW(TAG,
                 "server lost watchdog started wifi_cfg=%d eth=%d wifi=%d ppp=%d active=%d mode=%d",
                 s_wifi_config_ready ? 1 : 0,
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0,
                 (int)s_active_uplink,
                 (int)s_mode);
        return;
    }

    uint32_t lost_ms =
        (uint32_t)((now - s_server_lost_since_tick) * portTICK_PERIOD_MS);

    uint32_t log_ms =
        (uint32_t)((now - s_server_lost_last_log_tick) * portTICK_PERIOD_MS);

    if (log_ms >= APP_SERVER_LOST_LOG_MS) {
        s_server_lost_last_log_tick = now;

        ESP_LOGW(TAG,
                 "server lost for %lu ms threshold=%lu ms wifi_cfg=%d eth=%d wifi=%d ppp=%d active=%d mode=%d",
                 (unsigned long)lost_ms,
                 (unsigned long)APP_SERVER_LOST_RESET_MS,
                 s_wifi_config_ready ? 1 : 0,
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0,
                 (int)s_active_uplink,
                 (int)s_mode);
    }

    /*
     * Mất server quá lâu -> yêu cầu reset MCU.
     */
    if (lost_ms >= APP_SERVER_LOST_RESET_MS) {
        ESP_LOGE(TAG,
                 "server lost too long -> restart requested lost_ms=%lu",
                 (unsigned long)lost_ms);

        /*
         * Nếu đang bận thì chưa reset ngay.
         */
        if (netm_restart_should_defer()) {
            s_server_lost_reset_pending = true;
            s_server_lost_reset_pending_tick = now;

            ESP_LOGW(TAG,
                     "server lost reset pending, wait device idle");
            return;
        }

        ESP_LOGE(TAG,
                 "server lost too long -> restart MCU to recover network state");

        netm_mark_restart_reason(NETM_RESET_REASON_SERVER_LOST, lost_ms);

        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void handle_timers(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_mode == NETM_MODE_PROVISION ||
        s_active_uplink == APP_NET_AP_ONLY ||
        provision_service_is_running()) {
        return;
    }
    /*
     * 1. Retry WiFi thường
     */
    if (s_wifi_retry_due_tick != 0 && now >= s_wifi_retry_due_tick) {
        s_wifi_retry_due_tick = 0;

        if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
            ESP_LOGI(TAG, "skip Wi-Fi retry because Ethernet is online");
        }
        else if (s_wifi_has_ip || s_active_uplink == APP_NET_WIFI) {
            ESP_LOGI(TAG, "skip Wi-Fi retry because WiFi is already online");
        }
        else {
            bool no_uplink =
                (!s_eth_has_ip &&
                !s_wifi_has_ip &&
                !s_ppp_has_ip &&
                s_active_uplink == APP_NET_NONE &&
                s_mode == NETM_MODE_IDLE);

            if (no_uplink) {
                ESP_LOGI(TAG, "no uplink -> retry WiFi connect");
            } else {
                ESP_LOGW(TAG,
                        "wifi retry timer expired -> reconnect active=%d mode=%d ppp=%d",
                        (int)s_active_uplink,
                        (int)s_mode,
                        s_ppp_has_ip ? 1 : 0);
            }

            start_wifi_connect();
        }
    }
    /*
     * 2. Chờ settle sau khi đổi uplink rồi mới start MQTT
     */
    if (s_wifi_settle_deadline_tick != 0 &&
        now >= s_wifi_settle_deadline_tick) {

        s_wifi_settle_deadline_tick = 0;
        maybe_start_mqtt();
    }

    /*
     * 3. Retry MQTT theo lịch hẹn
     */
    if (s_mqtt_retry_due_tick != 0 && now >= s_mqtt_retry_due_tick) {
        s_mqtt_retry_due_tick = 0;

        if (s_active_uplink == APP_NET_PPP) {
            bool ppp_ip = s_ppp_has_ip || modem_service_is_ip_ready();

            if (!ppp_ip) {
                ESP_LOGW(TAG, "MQTT retry: PPP has no IP -> restart PPP instead");

                s_ppp_has_ip = false;
                s_net_ready = false;
                s_active_uplink = APP_NET_NONE;
                s_mqtt_ready = false;
                s_mqtt_start_pending = false;
                s_mqtt_uplink = APP_NET_NONE;
                s_mqtt_start_tick = 0;

                transition_mode(NETM_MODE_IDLE, "mqtt_retry_ppp_no_ip");

                s_ppp_retry_due_tick = now + pdMS_TO_TICKS(1000);
                //return;
            }
        }

        if (s_net_ready &&
            (s_active_uplink == APP_NET_ETH ||
            s_active_uplink == APP_NET_WIFI ||
            s_active_uplink == APP_NET_PPP)) {

            ESP_LOGI(TAG, "mqtt retry timer expired -> restart mqtt");
            maybe_start_mqtt();
        } else {
            ESP_LOGI(TAG,
                    "mqtt retry skipped net_ready=%d active=%d",
                    s_net_ready ? 1 : 0,
                    (int)s_active_uplink);
        }
    }

    //* 4. Khi đang ở 4G, định kỳ thử WiFi lại.

    if (s_wifi_retry_from_4g_tick != 0 &&
        now >= s_wifi_retry_from_4g_tick) {

        /*
        * Xóa timer trước, không tự đẩy lùi ngay tại đầu block.
        * Nếu cần hẹn lại thì hẹn ở cuối theo kết quả thực tế.
        */
        s_wifi_retry_from_4g_tick = 0;

        bool ppp_alive =
            (s_active_uplink == APP_NET_PPP ||
            s_mode == NETM_MODE_PPP_ONLINE ||
            s_ppp_has_ip ||
            modem_service_is_ip_ready());

        if (!s_eth_has_ip &&
            s_wifi_config_ready &&
            !s_wifi_has_ip &&
            ppp_alive) {

            ESP_LOGI(TAG, "WiFi recheck timer expired -> probe WiFi");

            if (!wifi_service_is_connected()) {
                start_wifi_probe_from_4g();
            } else {
                ESP_LOGI(TAG, "WiFi recheck skipped: WiFi already connected");
            }

            /*
            * Nếu probe chưa lên WiFi ngay, vẫn hẹn lần sau.
            * Nếu WiFi lên thật, nhánh APP_EVENT_NET_UP/WIFI sẽ clear tick.
            */
            if (!s_wifi_has_ip && s_active_uplink == APP_NET_PPP) {
                s_wifi_retry_from_4g_tick =
                    now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

                ESP_LOGI(TAG,
                        "WiFi recheck from 4G rescheduled in %lu ms",
                        (unsigned long)APP_WIFI_RECHECK_FROM_4G_MS);
            }
        } else {
            ESP_LOGI(TAG,
                    "WiFi recheck skipped eth=%d wifi_cfg=%d wifi_ip=%d active=%d mode=%d ppp_alive=%d",
                    s_eth_has_ip ? 1 : 0,
                    s_wifi_config_ready ? 1 : 0,
                    s_wifi_has_ip ? 1 : 0,
                    (int)s_active_uplink,
                    (int)s_mode,
                    ppp_alive ? 1 : 0);

            /*
            * Nếu vẫn đang PPP thì hẹn lại.
            * Nếu không còn PPP, không cần hẹn ở đây.
            */
            if (!s_eth_has_ip &&
                !s_wifi_has_ip &&
                s_wifi_config_ready &&
                ppp_alive) {

                s_wifi_retry_from_4g_tick =
                    now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);
            }
        }
    }
    /*
     * 5. PPP restart pending sau CS session.
     *
     * modem_service chỉ báo pending.
     * net_manager quyết định có cần kéo PPP lại hay không,
     * dựa trên ưu tiên Ethernet > WiFi > 4G.
     */
    if (modem_service_is_cs_session_active()) {
        ESP_LOGI(TAG, "PPP restart pending check delayed because CS session active");
    }
    else if (modem_service_take_ppp_restart_pending_after_cs()) {
        bool eth_ok  = s_eth_has_ip;
        bool wifi_ok = s_wifi_has_ip;
        bool ppp_ip  = s_ppp_has_ip || modem_service_is_ip_ready();
        bool mqtt_ok = mqtt_service_is_connected();

        ESP_LOGW(TAG,
                "PPP restart pending after CS -> decide eth=%d wifi=%d ppp_ip=%d mqtt=%d active=%d mode=%d",
                eth_ok ? 1 : 0,
                wifi_ok ? 1 : 0,
                ppp_ip ? 1 : 0,
                mqtt_ok ? 1 : 0,
                (int)s_active_uplink,
                (int)s_mode);

        /*
        * Nếu Ethernet/WiFi đã có thì không cần kéo PPP.
        */
        if (eth_ok || wifi_ok) {
            ESP_LOGI(TAG, "CS done: higher priority uplink exists -> no PPP restart");
            maybe_start_mqtt();
        }
        /*
        * Không có ETH/WIFI thì PPP là đường sống.
        * Nếu PPP chưa có IP thật hoặc MQTT chưa online thì phải kéo PPP/MQTT lại.
        */
        else if (!ppp_ip || !mqtt_ok) {
            ESP_LOGW(TAG, "CS done: no ETH/WIFI and PPP/MQTT not ready -> restart PPP now");

            /*
            * Clear state lệch do PPP bị stop trong CS session.
            */
            s_ppp_has_ip = false;
            s_net_ready = false;
            s_active_uplink = APP_NET_NONE;
            s_mqtt_ready = false;
            s_mqtt_start_pending = false;
            s_mqtt_uplink = APP_NET_NONE;
            s_mqtt_start_tick = 0;
            s_mqtt_retry_due_tick = 0;
            s_ppp_mqtt_recover_until_tick = 0;

            transition_mode(NETM_MODE_IDLE, "cs_done_restart_ppp");

            stop_ppp_connect();

            s_ppp_retry_due_tick = now + pdMS_TO_TICKS(1500);
        }
        else {
            ESP_LOGI(TAG, "CS done: PPP IP and MQTT OK");
        }
    }

    /*
     * 6. Retry PPP/4G
     */
    if (s_ppp_retry_due_tick != 0 && now >= s_ppp_retry_due_tick) {
        s_ppp_retry_due_tick = 0;

        if (s_eth_has_ip || s_wifi_has_ip) {
            ESP_LOGI(TAG,
                    "PPP retry skipped because higher priority uplink exists eth=%d wifi=%d",
                    s_eth_has_ip ? 1 : 0,
                    s_wifi_has_ip ? 1 : 0);
        }
        else if (s_ppp_has_ip || modem_service_is_ip_ready()) {
            ESP_LOGI(TAG,
                    "PPP retry skipped because PPP already has IP ppp=%d ip_ready=%d",
                    s_ppp_has_ip ? 1 : 0,
                    modem_service_is_ip_ready() ? 1 : 0);
        }
        else if (s_mode == NETM_MODE_PPP_CONNECTING) {
            ESP_LOGI(TAG, "PPP retry skipped because PPP is connecting");
        }
        else {
            ESP_LOGW(TAG,
                    "PPP retry timer expired -> retry 4G eth=%d wifi=%d ppp=%d active=%d mode=%d",
                    s_eth_has_ip ? 1 : 0,
                    s_wifi_has_ip ? 1 : 0,
                    s_ppp_has_ip ? 1 : 0,
                    (int)s_active_uplink,
                    (int)s_mode);

            s_net_ready = false;
            s_ppp_has_ip = false;
            s_active_uplink = APP_NET_NONE;
            s_mqtt_ready = false;
            s_mqtt_start_pending = false;
            s_mqtt_uplink = APP_NET_NONE;
            s_mqtt_start_tick = 0;
            s_mqtt_retry_due_tick = 0;
            s_ppp_mqtt_recover_until_tick = 0;

            transition_mode(NETM_MODE_IDLE, "ppp_retry_force");

            start_ppp_connect();
        }
    }

    /*
     * 7. MQTT watchdog.
     *
     * Chỉ chạy watchdog khi:
     * - Đã có network ready
     * - active_uplink hợp lệ
     * - MQTT chưa connected
     *
     * Không chạy khi vừa boot/chưa có mạng, tránh reset ESP sai.
     */
    if (s_net_ready &&
        (s_active_uplink == APP_NET_ETH ||
         s_active_uplink == APP_NET_WIFI ||
         s_active_uplink == APP_NET_PPP) &&
        !mqtt_service_is_connected()) {

        if (s_mqtt_down_since_tick == 0) {
            s_mqtt_down_since_tick = now;
        }

        uint32_t down_ms = pdTICKS_TO_MS(now - s_mqtt_down_since_tick);

        /*
         * Mất MQTT > 3 phút: stop/start MQTT.
         */
        if (down_ms >= APP_MQTT_RESTART_AFTER_MS &&
            (s_last_mqtt_recover_tick == 0 ||
             pdTICKS_TO_MS(now - s_last_mqtt_recover_tick) >= APP_MQTT_RESTART_AFTER_MS)) {

            ESP_LOGW(TAG,
                     "MQTT down %lu ms -> restart MQTT",
                     (unsigned long)down_ms);

            s_last_mqtt_recover_tick = now;

            s_mqtt_ready = false;
            s_mqtt_start_pending = false;
            s_mqtt_start_tick = 0;

            if (mqtt_service_is_started() || mqtt_service_is_connected()) {
                mqtt_service_stop();
            }

            s_mqtt_retry_due_tick = now + pdMS_TO_TICKS(1000);
        }

        /*
         * Mất MQTT > 5 phút: recover uplink hiện tại.
         */
        if (down_ms >= APP_UPLINK_RECOVER_AFTER_MS &&
            (s_last_uplink_recover_tick == 0 ||
             pdTICKS_TO_MS(now - s_last_uplink_recover_tick) >= APP_UPLINK_RECOVER_AFTER_MS)) {

            ESP_LOGW(TAG,
                     "MQTT down %lu ms -> recover uplink active=%d",
                     (unsigned long)down_ms,
                     (int)s_active_uplink);

            s_last_uplink_recover_tick = now;

            if (s_active_uplink == APP_NET_WIFI) {
                /*
                 * WiFi có IP nhưng MQTT mất lâu:
                 * stop WiFi STA, clear state, hẹn reconnect WiFi.
                 */
                wifi_service_stop_sta();

                s_wifi_has_ip = false;
                s_net_ready = false;
                s_active_uplink = APP_NET_NONE;

                s_wifi_retry_due_tick = now + pdMS_TO_TICKS(1000);
            }
            else if (s_active_uplink == APP_NET_PPP) {
                /*
                 * Đang PPP mà mất server lâu:
                 * ưu tiên thử WiFi lại, đồng thời hẹn retry PPP.
                 */
                if (s_wifi_config_ready) {
                    s_wifi_retry_from_4g_tick =
                        now + pdMS_TO_TICKS(1000);
                }

                s_ppp_retry_due_tick =
                    now + pdMS_TO_TICKS(1000);
            }
            else if (s_active_uplink == APP_NET_NONE) {
                /*
                 * Không có active uplink nhưng net_ready vẫn đang true bất thường:
                 * kéo lại cả WiFi và PPP.
                 */
                if (s_wifi_config_ready) {
                    s_wifi_retry_due_tick =
                        now + pdMS_TO_TICKS(1000);
                }

                s_ppp_retry_due_tick =
                    now + pdMS_TO_TICKS(5000);
            }
        }

        /*
         * Mất MQTT > 10 phút khi đang PPP: restart modem.
         */
        if (down_ms >= APP_MODEM_RECOVER_AFTER_MS &&
            s_active_uplink == APP_NET_PPP &&
            (s_last_modem_recover_tick == 0 ||
             pdTICKS_TO_MS(now - s_last_modem_recover_tick) >= APP_MODEM_RECOVER_AFTER_MS)) {

            ESP_LOGW(TAG,
                     "MQTT down %lu ms on PPP -> restart modem",
                     (unsigned long)down_ms);

            s_last_modem_recover_tick = now;

            modem_service_power_cycle_and_restart();
        }

        /*
         * Mất server quá lâu: reset ESP.
         */
        // if (down_ms >= APP_FORCE_REBOOT_NO_SERVER_MS) {
        //     ESP_LOGE(TAG,
        //              "MQTT down too long %lu ms -> restart ESP",
        //              (unsigned long)down_ms);

        //     esp_restart();
        // }
    } else {
        /*
         * Nếu MQTT đã connected hoặc chưa có network ready,
         * clear mốc đếm down để lần sau tính lại từ đầu.
         */
        s_mqtt_down_since_tick = 0;
    }
    // netm_server_lost_watchdog(now);
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

            TickType_t now = xTaskGetTickCount();

            if (st->type == APP_NET_ETH) {
                ESP_LOGI(TAG, "Ethernet uplink online");

                s_eth_has_ip = true;
                s_wifi_start_after_eth_wait = false;
                s_eth_wait_deadline_tick = 0;

                /*
                * Ethernet ưu tiên cao nhất.
                */
                s_net_ready = true;
                s_active_uplink = APP_NET_ETH;

                netm_reset_mqtt_state("Ethernet up");

                /*
                * Ethernet online thì dừng WiFi STA.
                */
                wifi_service_set_reconnect_enabled(false);
                wifi_service_stop_sta();

                s_wifi_has_ip = false;
                s_wifi_lost_tick = 0;
                s_wifi_retry_due_tick = 0;
                s_wifi_online_since_tick = 0;
                s_wifi_retry_from_4g_tick = 0;
                s_wifi_connect_start_tick = 0;

                /*
                * Ethernet online thì dừng PPP nếu đang có.
                */
                if (s_mode == NETM_MODE_PPP_ONLINE ||
                    s_mode == NETM_MODE_PPP_CONNECTING ||
                    s_active_uplink == APP_NET_PPP ||
                    s_ppp_has_ip) {

                    ESP_LOGW(TAG, "Ethernet is back -> stop PPP");

                    stop_ppp_connect();
                    s_ppp_has_ip = false;
                }

                s_ppp_retry_due_tick = 0;

                transition_mode(NETM_MODE_ETH_ONLINE, "eth_net_up");

                /*
                * Cho stop MQTT/WiFi/PPP settle xong rồi mới start MQTT Ethernet.
                */
                s_wifi_settle_deadline_tick =
                    now + pdMS_TO_TICKS(2000);

                ESP_LOGI(TAG, "Ethernet MQTT start scheduled");
            }

            else if (st->type == APP_NET_WIFI) {
                ESP_LOGI(TAG, "Wi-Fi uplink online");

                s_wifi_has_ip = true;
                s_wifi_online_since_tick = now;

                if (s_eth_has_ip) {
                    ESP_LOGI(TAG, "ignore Wi-Fi because Ethernet is online");
                    s_wifi_retry_due_tick = 0;
                    s_wifi_lost_tick = 0;
                    s_wifi_connect_start_tick = 0;
                    s_wifi_retry_from_4g_tick = 0;

                    break;
                }

                tm1638_wifi_set_state(WIFI_LED_READY);

                bool was_ppp = (s_mode == NETM_MODE_PPP_ONLINE ||
                                s_mode == NETM_MODE_PPP_CONNECTING ||
                                s_active_uplink == APP_NET_PPP ||
                                s_ppp_has_ip
                                || modem_service_is_ip_ready());

                /*
                * WiFi ưu tiên cao hơn PPP.
                */
                if (was_ppp) {
                    ESP_LOGW(TAG, "Wi-Fi recovered -> switch back from 4G");

                    netm_reset_mqtt_state("WiFi up, switch from 4G to WiFi");

                    stop_ppp_connect();

                    s_ppp_has_ip = false;
                    s_ppp_retry_due_tick = 0;
                    s_ppp_mqtt_recover_until_tick = 0;
                } else {
                    netm_reset_mqtt_state("WiFi up, restart MQTT on WiFi");
                }

                //s_wifi_has_ip = true;
                s_net_ready = true;
                s_active_uplink = APP_NET_WIFI;

                s_wifi_retry_due_tick = 0;
                s_wifi_lost_tick = 0;
                s_wifi_connect_start_tick = 0;
                s_wifi_retry_from_4g_tick = 0;

                //s_wifi_online_since_tick = now;

                /*
                * WiFi đã online thì không cần retry PPP.
                */
                s_ppp_retry_due_tick = 0;

                s_wifi_settle_deadline_tick =
                    now + pdMS_TO_TICKS(APP_WIFI_SETTLE_BEFORE_MQTT_MS);

                transition_mode(NETM_MODE_WIFI_ONLINE, "wifi_net_up");

                /*
                * Gọi cũng được, maybe_start_mqtt sẽ tự return nếu còn settle.
                */
                maybe_start_mqtt();
            }

            else if (st->type == APP_NET_PPP) {
                ESP_LOGI(TAG, "PPP uplink online");

                s_ppp_has_ip = true;
                s_ppp_retry_due_tick = 0;

                /*
                * PPP là ưu tiên thấp nhất.
                * Nếu Ethernet online thì bỏ PPP.
                */
                if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
                    ESP_LOGW(TAG, "ignore PPP because Ethernet is online");

                    stop_ppp_connect();
                    s_ppp_has_ip = false;
                    s_ppp_retry_due_tick = 0;
                    s_ppp_mqtt_recover_until_tick = 0;

                    s_net_ready = true;
                    s_active_uplink = APP_NET_ETH;

                    if (!mqtt_service_is_connected() && !s_mqtt_start_pending) {
                        s_mqtt_retry_due_tick =
                            now + pdMS_TO_TICKS(1000);

                        ESP_LOGI(TAG, "schedule MQTT over Ethernet after ignoring PPP");
                    }

                    break;
                }

                /*
                * Nếu WiFi đã có IP thì không cho PPP ghi đè.
                */
                if (s_wifi_has_ip) {
                    ESP_LOGW(TAG, "ignore PPP because Wi-Fi has IP");

                    stop_ppp_connect();
                    s_ppp_has_ip = false;
                    s_ppp_retry_due_tick = 0;
                    s_ppp_mqtt_recover_until_tick = 0;

                    s_net_ready = true;
                    s_active_uplink = APP_NET_WIFI;

                    if (!mqtt_service_is_connected() && !s_mqtt_start_pending) {
                        s_mqtt_retry_due_tick =
                            now + pdMS_TO_TICKS(1000);

                        ESP_LOGI(TAG, "schedule MQTT over WiFi after ignoring PPP");
                    }

                    break;
                }

                /*
                * Lúc này không có Ethernet/WiFi, chấp nhận PPP.
                */
                netm_reset_mqtt_state("PPP up, switch to 4G");

                s_net_ready = true;
                s_active_uplink = APP_NET_PPP;

                if (s_wifi_config_ready) {
                    if (s_wifi_retry_from_4g_tick == 0) {
                        s_wifi_retry_from_4g_tick =
                            now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

                        ESP_LOGI(TAG,
                                "WiFi recheck from 4G scheduled in %lu ms",
                                (unsigned long)APP_WIFI_RECHECK_FROM_4G_MS);
                    } else {
                        ESP_LOGI(TAG,
                                "WiFi recheck from 4G already scheduled, keep old schedule");
                    }
                }

                transition_mode(NETM_MODE_PPP_ONLINE, "ppp_net_up");

                /*
                * PPP vừa GOT IP, cho route/DNS/socket settle rồi mới start MQTT.
                */
                s_wifi_settle_deadline_tick =
                    now + pdMS_TO_TICKS(APP_PPP_SETTLE_BEFORE_MQTT_MS);

                /*
                * PPP vừa GOT IP, trong khoảng guard này không cho SMS poll cắt PPP.
                */
                s_ppp_mqtt_recover_until_tick =
                    now + pdMS_TO_TICKS(APP_PPP_MQTT_RECOVER_GUARD_MS);

                ESP_LOGI(TAG,
                        "PPP MQTT start scheduled after settle, recover guard=%lu ms",
                        (unsigned long)APP_PPP_MQTT_RECOVER_GUARD_MS);
            }

            break;
        }

        case APP_EVENT_NET_DOWN: {
            app_net_status_t *st = (app_net_status_t *)data;
            if (!st) {
                break;
            }
            TickType_t now = xTaskGetTickCount();

             if (st->type == APP_NET_ETH) {
                ESP_LOGW(TAG, "Ethernet uplink down");

                s_eth_has_ip = false;

                if (s_active_uplink == APP_NET_ETH) {
                    s_net_ready = false;

                    netm_reset_mqtt_state("Ethernet down");

                    s_active_uplink = APP_NET_NONE;
                    s_ppp_mqtt_recover_until_tick = 0;
                    transition_mode(NETM_MODE_IDLE, "eth_net_down");

                    if (s_wifi_config_ready) {
                        s_wifi_lost_tick = now;
                        s_wifi_retry_due_tick = now + pdMS_TO_TICKS(1000);
                        s_ppp_retry_due_tick = 0;
                        ESP_LOGI(TAG, "schedule Wi-Fi start after Ethernet down");
                    } else {
                        request_enter_provision_mode("eth_down_wifi_config_missing");
                    }
                }
            }
            else if (st->type == APP_NET_WIFI) {

                ESP_LOGW(TAG, "Wi-Fi uplink down");

                s_wifi_has_ip = false;
                s_wifi_online_since_tick = 0;
                s_wifi_settle_deadline_tick = 0;

                tm1638_wifi_set_state(WIFI_LED_OFF);

                if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
                    ESP_LOGI(TAG, "ignore Wi-Fi down because Ethernet is online");
                    s_wifi_lost_tick = 0;
                    s_wifi_retry_due_tick = 0;
                    break;
                }

                /*
                * Nếu đang dùng PPP/4G rồi thì WiFi down không được kéo state về WIFI_CONNECTING.
                */
                if (s_active_uplink == APP_NET_PPP ||
                    s_mode == NETM_MODE_PPP_ONLINE ||
                    s_mode == NETM_MODE_PPP_CONNECTING) {

                    ESP_LOGI(TAG, "Wi-Fi down while PPP active/connecting -> stop WiFi STA and wait periodic recheck");

                    wifi_service_set_reconnect_enabled(false);
                    wifi_service_stop_sta();

                    s_wifi_lost_tick = 0;
                    s_wifi_retry_due_tick = 0;
                    s_wifi_connect_start_tick = 0;
                    s_wifi_online_since_tick = 0;
                    s_wifi_has_ip = false;
                    s_ppp_retry_due_tick = 0;

                    // if (s_wifi_config_ready) {
                    //     s_wifi_retry_from_4g_tick =
                    //         now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

                    //     ESP_LOGI(TAG,
                    //             "WiFi recheck from 4G scheduled in %lu ms",
                    //             (unsigned long)APP_WIFI_RECHECK_FROM_4G_MS);
                    // }
                    if (s_wifi_config_ready) {
                        if (s_wifi_retry_from_4g_tick == 0) {
                            s_wifi_retry_from_4g_tick =
                                now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

                            ESP_LOGI(TAG,
                                    "WiFi recheck from 4G scheduled in %lu ms",
                                    (unsigned long)APP_WIFI_RECHECK_FROM_4G_MS);
                        } else {
                            ESP_LOGI(TAG,
                                    "WiFi recheck from 4G already scheduled, keep old schedule");
                        }
                    }
                    break;
                }

                if (s_active_uplink == APP_NET_WIFI) {
                    s_net_ready = false;

                    netm_reset_mqtt_state("WiFi down");

                    s_active_uplink = APP_NET_NONE;
                    s_ppp_mqtt_recover_until_tick = 0;

                    s_wifi_connect_start_tick = 0;
                    s_wifi_retry_from_4g_tick = 0;
                    if (s_wifi_lost_tick == 0) {
                        s_wifi_lost_tick = now;
                    }

                    transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_net_down");

                    s_wifi_retry_due_tick = now + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);

                    /*
                    * Hẹn fallback sang PPP nếu WiFi mất lâu.
                    * Nếu handle_timers đã có logic dựa vào s_wifi_lost_tick thì không cần gọi PPP ở đây.
                    */
                    if (!s_eth_has_ip) {
                        s_ppp_retry_due_tick = now + pdMS_TO_TICKS(APP_WIFI_LOST_TO_4G_MS);

                        ESP_LOGW(TAG,
                                "WiFi down -> PPP fallback scheduled in %lu ms",
                                (unsigned long)APP_WIFI_LOST_TO_4G_MS);
                    }
                }

                else if (s_active_uplink == APP_NET_NONE &&
                        !s_eth_has_ip &&
                        !s_wifi_has_ip &&
                        !s_ppp_has_ip) {

                    if (s_wifi_lost_tick == 0) {
                        s_wifi_lost_tick = now;
                    }

                    transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_down_no_active");

                    s_wifi_retry_due_tick =
                        now + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);

                    s_ppp_retry_due_tick =
                        now + pdMS_TO_TICKS(APP_WIFI_LOST_TO_4G_MS);

                    ESP_LOGW(TAG,
                            "WiFi down no active uplink -> PPP fallback scheduled in %lu ms",
                            (unsigned long)APP_WIFI_LOST_TO_4G_MS);
                }
            }
            else if (st->type == APP_NET_PPP) {
                ESP_LOGW(TAG, "PPP uplink down");

                s_ppp_has_ip = false;
                s_ppp_mqtt_recover_until_tick = 0;
                if (s_active_uplink == APP_NET_PPP) {
                    s_net_ready = false;

                    netm_reset_mqtt_state("PPP down");

                    s_active_uplink = APP_NET_NONE;
                    transition_mode(NETM_MODE_IDLE, "ppp_net_down");
                }

                /*
                * Nếu PPP down do CS session thì chờ pending restart sau CS.
                * Không schedule PPP reconnect ngay tại đây.
                */
                if (!s_eth_has_ip &&
                    !s_wifi_has_ip &&
                    !modem_service_is_cs_session_active()) {

                    s_ppp_retry_due_tick = now + pdMS_TO_TICKS(5000);

                    ESP_LOGW(TAG, "PPP down -> schedule PPP reconnect");

                    if (s_wifi_config_ready) {
                        s_wifi_retry_from_4g_tick =
                            now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

                        ESP_LOGW(TAG,
                                "PPP down -> also schedule WiFi recheck in %lu ms",
                                (unsigned long)APP_WIFI_RECHECK_FROM_4G_MS);
                    }
                }
                else if (modem_service_is_cs_session_active()) {
                    ESP_LOGI(TAG, "PPP down during CS session -> wait pending restart after CS");
                }
                else {
                    s_ppp_retry_due_tick = 0;

                    ESP_LOGI(TAG,
                            "PPP down ignored because higher priority uplink exists eth=%d wifi=%d",
                            s_eth_has_ip ? 1 : 0,
                            s_wifi_has_ip ? 1 : 0);
                }
            }
            break;
        }

        case APP_EVENT_MQTT_CONNECTED:
        {
            ESP_LOGI(TAG, "MQTT connected");

            s_mqtt_ready = true;
            s_mqtt_start_pending = false;
            s_mqtt_retry_due_tick = 0;
            s_mqtt_start_tick = 0;
            s_mqtt_down_since_tick = 0;
            s_mqtt_uplink = s_active_uplink;

            s_ppp_mqtt_recover_until_tick = 0;
            /*
            * Clear các mốc recover nếu anh có dùng watchdog.
            */
            s_last_mqtt_recover_tick = 0;
            s_last_uplink_recover_tick = 0;
            s_last_modem_recover_tick = 0;

            tm1638_server_set_state(SERVER_LED_CONNECTED);

            ESP_LOGW(TAG,
                    "MQTT connected OK, active_uplink=%d mqtt_uplink=%d",
                    (int)s_active_uplink,
                    (int)s_mqtt_uplink);

            break;
        }

        case APP_EVENT_MQTT_DISCONNECTED:
        {
            TickType_t now = xTaskGetTickCount();

            ESP_LOGW(TAG, "MQTT disconnected");

            s_mqtt_ready = false;
            s_mqtt_start_pending = false;
            s_mqtt_start_tick = 0;

            if (s_mqtt_down_since_tick == 0) {
                s_mqtt_down_since_tick = now;
            }

            bool mqtt_down_during_modem_cs =
                (s_active_uplink == APP_NET_PPP) &&
                (modem_service_is_cs_session_active() ||
                !modem_service_should_auto_restart_ppp());

            /*
            * Neu MQTT mat do PPP dang vao CS session de doc/gui SMS,
            * day la gian doan chu dong.
            * Khong bao server LED loi ngay.
            */
            if (mqtt_down_during_modem_cs) {
                ESP_LOGI(TAG,
                        "MQTT disconnected during modem CS session, keep server LED and delay retry");

                if (mqtt_service_is_started() || mqtt_service_is_connected()) {
                    mqtt_service_stop();
                }

                /*
                * Van giu active_uplink = PPP.
                * Nhung MQTT client thi clear de lan sau start sach.
                */
                s_mqtt_uplink = APP_NET_NONE;

                s_mqtt_retry_due_tick =
                    now + pdMS_TO_TICKS(20000);

                ESP_LOGW(TAG, "MQTT PPP/CS retry scheduled in 20000 ms");
                break;
            }

            /*
            * MQTT mat that su thi bao LED server loi.
            */
            tm1638_server_set_state(SERVER_LED_ERROR_BLINK);

            /*
            * Stop MQTT client cu cho sach.
            * Neu khong stop, lan sau co the bi ket mqtt_service_is_started().
            */
            if (mqtt_service_is_started() || mqtt_service_is_connected()) {
                mqtt_service_stop();
            }

            s_mqtt_uplink = APP_NET_NONE;

            /*
            * PPP/4G retry cham hon Ethernet/WiFi.
            */
            if (s_active_uplink == APP_NET_PPP) {
                s_mqtt_retry_due_tick =
                    now + pdMS_TO_TICKS(20000);

                /*
                * Quan trong:
                * Neu dang dung PPP ma MQTT mat, van phai hen kiem tra WiFi lai.
                * De tranh WiFi OFF mai, 4G nhap nhay mai.
                */
                if (s_wifi_config_ready) {
                    s_wifi_retry_from_4g_tick =
                        now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);
                }

                ESP_LOGW(TAG, "MQTT PPP disconnected -> retry in 20000 ms");
                break;
            }

            /*
            * Ethernet/WiFi retry nhanh hon.
            */
            if (s_net_ready &&
                (s_active_uplink == APP_NET_ETH ||
                s_active_uplink == APP_NET_WIFI)) {

                s_mqtt_retry_due_tick =
                    now + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);

                ESP_LOGW(TAG,
                        "MQTT disconnected -> retry in %lu ms",
                        (unsigned long)APP_WIFI_RETRY_DELAY_MS);

                break;
            }

            /*
            * Neu khong ro active uplink nao, hen retry nhe.
            */
            s_mqtt_retry_due_tick =
                now + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);

            ESP_LOGW(TAG, "MQTT disconnected with no active uplink -> retry later");

            break;
        }

        case APP_EVENT_WIFI_CONFIG_SAVED:
            ESP_LOGI(TAG, "Wi-Fi config saved -> reload and reconnect");

            s_wifi_config_ready = load_runtime_cfg();

            s_wifi_retry_due_tick = 0;
            s_wifi_retry_from_4g_tick = 0;
            s_wifi_lost_tick = 0;
            s_wifi_connect_start_tick = 0;
            s_wifi_online_since_tick = 0;
            s_wifi_has_ip = false;

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
            (now - s_wifi_lost_tick) >= pdMS_TO_TICKS(APP_WIFI_LOST_TO_4G_MS)) {

            s_wifi_lost_tick = 0;

            /*
            * Khi chuyển sang 4G, dù PPP chưa online vẫn phải hẹn giờ
            * kiểm tra lại WiFi sau APP_WIFI_RECHECK_FROM_4G_MS.
            */
            s_wifi_retry_from_4g_tick =
                now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);

            ESP_LOGW(TAG, "Wi-Fi lost too long -> switch to 4G");
            start_ppp_connect();
        }
        if (s_mode == NETM_MODE_PPP_ONLINE && s_wifi_retry_from_4g_tick == 0) {
            s_wifi_retry_from_4g_tick = now + pdMS_TO_TICKS(APP_WIFI_RECHECK_FROM_4G_MS);
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
