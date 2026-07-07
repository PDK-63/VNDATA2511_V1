#include "app_recovery.h"

#include "app_events.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "modem_service.h"
#include "runtime_config.h"

#include <string.h>

static const char *TAG = "app_recovery";

#ifndef APP_RECOVERY_TASK_STACK
#define APP_RECOVERY_TASK_STACK 4096
#endif

#ifndef APP_RECOVERY_TASK_PRIO
#define APP_RECOVERY_TASK_PRIO 6
#endif

/*
 * Timeline phục hồi.
 * Có thể chỉnh lại theo yêu cầu khách.
 */
#ifndef APP_RECOVERY_MQTT_RESTART_MS
#define APP_RECOVERY_MQTT_RESTART_MS       (3UL  * 60UL * 1000UL)
#endif

#ifndef APP_RECOVERY_UPLINK_RECOVER_MS
#define APP_RECOVERY_UPLINK_RECOVER_MS     (5UL  * 60UL * 1000UL)
#endif

#ifndef APP_RECOVERY_MODEM_RECOVER_MS
#define APP_RECOVERY_MODEM_RECOVER_MS      (7UL  * 60UL * 1000UL)
#endif

#ifndef APP_RECOVERY_FORCE_REBOOT_MS
#define APP_RECOVERY_FORCE_REBOOT_MS       (10UL * 60UL * 1000UL)
#endif

#ifndef APP_RECOVERY_LOG_INTERVAL_MS
#define APP_RECOVERY_LOG_INTERVAL_MS       (60UL * 1000UL)
#endif

#define APP_RECOVERY_RESET_MAGIC           0x52435659UL

#ifndef APP_RECOVERY_BOOT_GRACE_MS
#define APP_RECOVERY_BOOT_GRACE_MS         (120UL * 1000UL)
#endif
static TickType_t s_boot_tick = 0;

typedef enum {
    APP_RECOVERY_REBOOT_NONE = 0,
    APP_RECOVERY_REBOOT_SERVER_LOST,
    APP_RECOVERY_REBOOT_WIFI_STUCK,
    APP_RECOVERY_REBOOT_MODEM_STUCK,
} app_recovery_reboot_reason_t;

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t lost_ms;
    uint32_t active_uplink;
    uint32_t eth_has_ip;
    uint32_t wifi_has_ip;
    uint32_t ppp_has_ip;
} app_recovery_reset_marker_t;

static RTC_NOINIT_ATTR app_recovery_reset_marker_t s_reset_marker;

static TaskHandle_t s_task = NULL;
static bool s_started = false;

static TickType_t s_boot_grace_start_tick = 0;
static bool s_boot_grace_logged = false;

/*
 * Trạng thái mạng được app_recovery tự cập nhật bằng event.
 * Không cần đọc biến static trong net_manager.c.
 */
static bool s_eth_has_ip = false;
static bool s_wifi_has_ip = false;
static bool s_ppp_has_ip = false;
static app_net_type_t s_active_uplink = APP_NET_NONE;

static bool s_wifi_config_ready = false;
static bool s_provision_active = false;

/*
 * Wi-Fi reason lấy từ WIFI_EVENT_STA_DISCONNECTED.
 * Không cần sửa wifi_service.c.
 */
static bool s_wifi_no_ap_found = false;
static bool s_wifi_auth_fail = false;
static TickType_t s_wifi_stuck_since_tick = 0;

/*
 * Server/MQTT watchdog.
 */
static TickType_t s_server_lost_since_tick = 0;
static TickType_t s_server_lost_last_log_tick = 0;
static TickType_t s_last_mqtt_recover_tick = 0;
static TickType_t s_last_uplink_recover_tick = 0;
static TickType_t s_last_modem_recover_tick = 0;

/*
 * Pending reset.
 */
static bool s_reboot_pending = false;
static app_recovery_reboot_reason_t s_reboot_reason = APP_RECOVERY_REBOOT_NONE;
static uint32_t s_reboot_lost_ms = 0;

/*
 * Busy flags.
 */
static bool s_busy_flags[APP_RECOVERY_BUSY_MAX];

static const char *reboot_reason_name(app_recovery_reboot_reason_t reason)
{
    switch (reason) {
    case APP_RECOVERY_REBOOT_SERVER_LOST:
        return "SERVER_LOST";
    case APP_RECOVERY_REBOOT_WIFI_STUCK:
        return "WIFI_STUCK";
    case APP_RECOVERY_REBOOT_MODEM_STUCK:
        return "MODEM_STUCK";
    default:
        return "NONE";
    }
}

static const char *uplink_name(app_net_type_t type)
{
    switch (type) {
    case APP_NET_ETH:
        return "ETH";
    case APP_NET_WIFI:
        return "WIFI";
    case APP_NET_PPP:
        return "PPP";
    case APP_NET_AP_ONLY:
        return "AP_ONLY";
    default:
        return "NONE";
    }
}

void app_recovery_set_busy(app_recovery_busy_reason_t reason, bool busy)
{
    if (reason < 0 || reason >= APP_RECOVERY_BUSY_MAX) {
        return;
    }

    s_busy_flags[reason] = busy;

    ESP_LOGW(TAG,
             "busy flag %d -> %d",
             (int)reason,
             busy ? 1 : 0);
}

static bool recovery_is_busy(void)
{
    if (s_provision_active) {
        ESP_LOGW(TAG, "reboot deferred: provision active");
        return true;
    }

    if (modem_service_is_cs_session_active()) {
        ESP_LOGW(TAG, "reboot deferred: modem CS session active");
        return true;
    }

    for (int i = 0; i < APP_RECOVERY_BUSY_MAX; i++) {
        if (s_busy_flags[i]) {
            ESP_LOGW(TAG, "reboot deferred: busy flag=%d", i);
            return true;
        }
    }

    return false;
}

static void mark_reset_reason(app_recovery_reboot_reason_t reason, uint32_t lost_ms)
{
    s_reset_marker.magic = APP_RECOVERY_RESET_MAGIC;
    s_reset_marker.reason = (uint32_t)reason;
    s_reset_marker.lost_ms = lost_ms;
    s_reset_marker.active_uplink = (uint32_t)s_active_uplink;
    s_reset_marker.eth_has_ip = s_eth_has_ip ? 1 : 0;
    s_reset_marker.wifi_has_ip = s_wifi_has_ip ? 1 : 0;
    s_reset_marker.ppp_has_ip = s_ppp_has_ip ? 1 : 0;
}

static void request_reboot(app_recovery_reboot_reason_t reason, uint32_t lost_ms)
{
    if (s_reboot_pending) {
        return;
    }

    s_reboot_pending = true;
    s_reboot_reason = reason;
    s_reboot_lost_ms = lost_ms;

    ESP_LOGE(TAG,
             "reboot requested reason=%s lost_ms=%lu active=%s eth=%d wifi=%d ppp=%d",
             reboot_reason_name(reason),
             (unsigned long)lost_ms,
             uplink_name(s_active_uplink),
             s_eth_has_ip ? 1 : 0,
             s_wifi_has_ip ? 1 : 0,
             s_ppp_has_ip ? 1 : 0);
}

static void handle_pending_reboot(void)
{
    if (!s_reboot_pending) {
        return;
    }

    if (recovery_is_busy()) {
        return;
    }

    ESP_LOGE(TAG,
             "system reboot now reason=%s lost_ms=%lu",
             reboot_reason_name(s_reboot_reason),
             (unsigned long)s_reboot_lost_ms);

    mark_reset_reason(s_reboot_reason, s_reboot_lost_ms);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static bool has_any_network_config_or_link(void)
{
    if (s_wifi_config_ready) {
        return true;
    }

    if (s_eth_has_ip || s_wifi_has_ip || s_ppp_has_ip) {
        return true;
    }

    return false;
}

static void clear_server_watchdog(void)
{
    s_server_lost_since_tick = 0;
    s_server_lost_last_log_tick = 0;
    s_last_mqtt_recover_tick = 0;
    s_last_uplink_recover_tick = 0;
    s_last_modem_recover_tick = 0;
    s_reboot_pending = false;
    s_reboot_reason = APP_RECOVERY_REBOOT_NONE;
    s_reboot_lost_ms = 0;
}

static void handle_server_watchdog(TickType_t now)
{
    if (mqtt_service_is_connected()) {
        clear_server_watchdog();
        return;
    }

    /*
     * Đang setting Wi-Fi thì không tính mất server.
     */
    if (s_provision_active) {
        s_server_lost_since_tick = 0;
        return;
    }

    /*
     * Nếu chưa có bất kỳ cấu hình/kênh mạng nào thì reset không giúp gì.
     */
    if (!has_any_network_config_or_link()) {
        s_server_lost_since_tick = 0;
        return;
    }

    if (s_server_lost_since_tick == 0) {
        s_server_lost_since_tick = now;
        s_server_lost_last_log_tick = now;

        ESP_LOGW(TAG,
                 "server watchdog started active=%s eth=%d wifi=%d ppp=%d wifi_cfg=%d",
                 uplink_name(s_active_uplink),
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0,
                 s_wifi_config_ready ? 1 : 0);
        return;
    }

    uint32_t lost_ms = pdTICKS_TO_MS(now - s_server_lost_since_tick);
    uint32_t log_ms = pdTICKS_TO_MS(now - s_server_lost_last_log_tick);

    if (log_ms >= APP_RECOVERY_LOG_INTERVAL_MS) {
        s_server_lost_last_log_tick = now;

        ESP_LOGW(TAG,
                 "server lost %lu ms active=%s eth=%d wifi=%d ppp=%d",
                 (unsigned long)lost_ms,
                 uplink_name(s_active_uplink),
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0);
    }

    /*
     * Mất server 3 phút:
     * Dọn MQTT client cũ để net_manager tự start lại.
     */
    if (lost_ms >= APP_RECOVERY_MQTT_RESTART_MS &&
        (s_last_mqtt_recover_tick == 0 ||
         pdTICKS_TO_MS(now - s_last_mqtt_recover_tick) >= APP_RECOVERY_MQTT_RESTART_MS)) {

        s_last_mqtt_recover_tick = now;

        ESP_LOGW(TAG,
                 "server lost %lu ms -> stop MQTT for clean restart",
                 (unsigned long)lost_ms);

        mqtt_service_stop();
    }

    /*
     * Mất server 5 phút:
     * Không gọi sâu vào net_manager để tránh đụng logic cũ.
     * Chỉ stop MQTT thêm lần nữa, net_manager sẽ retry theo flow hiện có.
     */
    if (lost_ms >= APP_RECOVERY_UPLINK_RECOVER_MS &&
        (s_last_uplink_recover_tick == 0 ||
         pdTICKS_TO_MS(now - s_last_uplink_recover_tick) >= APP_RECOVERY_UPLINK_RECOVER_MS)) {

        s_last_uplink_recover_tick = now;

        ESP_LOGW(TAG,
                 "server lost %lu ms -> uplink recover hint active=%s",
                 (unsigned long)lost_ms,
                 uplink_name(s_active_uplink));

        mqtt_service_stop();
    }

    /*
     * Mất server lâu khi đang PPP:
     * Reset modem trước khi reset toàn bộ MCU.
     * Hàm này project bạn đã có dùng trong net_manager.
     */
    if (lost_ms >= APP_RECOVERY_MODEM_RECOVER_MS &&
        s_active_uplink == APP_NET_PPP &&
        (s_last_modem_recover_tick == 0 ||
         pdTICKS_TO_MS(now - s_last_modem_recover_tick) >= APP_RECOVERY_MODEM_RECOVER_MS)) {

        s_last_modem_recover_tick = now;

        if (!modem_service_is_cs_session_active()) {
            ESP_LOGW(TAG,
                     "server lost %lu ms on PPP -> modem power cycle",
                     (unsigned long)lost_ms);

            modem_service_power_cycle_and_restart();
        } else {
            ESP_LOGW(TAG,
                     "skip modem power cycle because CS session active");
        }
    }

    /*
     * Mất server 10 phút:
     * Yêu cầu reset thiết bị.
     * Nếu đang bận thì pending, xử lý xong mới reset.
     */
    if (lost_ms >= APP_RECOVERY_FORCE_REBOOT_MS) {
        request_reboot(APP_RECOVERY_REBOOT_SERVER_LOST, lost_ms);
    }
}

static void handle_wifi_stuck_watchdog(TickType_t now)
{
    /*
     * Chỉ xử lý Wi-Fi stuck khi:
     * - Có cấu hình Wi-Fi
     * - Chưa có Wi-Fi IP
     * - Không phải đang provision
     * - Không phải sai mật khẩu
     * - Không phải router không thấy
     */
    if (!s_wifi_config_ready ||
        s_wifi_has_ip ||
        s_provision_active ||
        s_wifi_auth_fail ||
        s_wifi_no_ap_found) {

        s_wifi_stuck_since_tick = 0;
        return;
    }

    /*
     * Nếu Ethernet đang online thì Wi-Fi không quan trọng.
     */
    if (s_eth_has_ip || s_active_uplink == APP_NET_ETH) {
        s_wifi_stuck_since_tick = 0;
        return;
    }

    if (s_wifi_stuck_since_tick == 0) {
        s_wifi_stuck_since_tick = now;

        ESP_LOGW(TAG,
                 "wifi stuck watchdog started active=%s ppp=%d",
                 uplink_name(s_active_uplink),
                 s_ppp_has_ip ? 1 : 0);
        return;
    }

    uint32_t stuck_ms = pdTICKS_TO_MS(now - s_wifi_stuck_since_tick);

    if (stuck_ms >= APP_RECOVERY_FORCE_REBOOT_MS) {
        ESP_LOGE(TAG,
                 "wifi stuck too long %lu ms -> reboot request",
                 (unsigned long)stuck_ms);

        request_reboot(APP_RECOVERY_REBOOT_WIFI_STUCK, stuck_ms);
    }
}

static void app_event_handler(void *arg,
                              esp_event_base_t base,
                              int32_t event_id,
                              void *data)
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
            s_eth_has_ip = true;
            s_active_uplink = APP_NET_ETH;
        } else if (st->type == APP_NET_WIFI) {
            s_wifi_has_ip = true;
            s_active_uplink = APP_NET_WIFI;

            s_wifi_no_ap_found = false;
            s_wifi_auth_fail = false;
            s_wifi_stuck_since_tick = 0;
        } else if (st->type == APP_NET_PPP) {
            s_ppp_has_ip = true;
            s_active_uplink = APP_NET_PPP;
        }

        ESP_LOGI(TAG,
                 "event NET_UP type=%d active=%s eth=%d wifi=%d ppp=%d",
                 (int)st->type,
                 uplink_name(s_active_uplink),
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0);
        break;
    }

    case APP_EVENT_NET_DOWN: {
        app_net_status_t *st = (app_net_status_t *)data;
        if (!st) {
            break;
        }

        if (st->type == APP_NET_ETH) {
            s_eth_has_ip = false;
            if (s_active_uplink == APP_NET_ETH) {
                s_active_uplink = APP_NET_NONE;
            }
        } else if (st->type == APP_NET_WIFI) {
            s_wifi_has_ip = false;
            if (s_active_uplink == APP_NET_WIFI) {
                s_active_uplink = APP_NET_NONE;
            }
        } else if (st->type == APP_NET_PPP) {
            s_ppp_has_ip = false;
            if (s_active_uplink == APP_NET_PPP) {
                s_active_uplink = APP_NET_NONE;
            }
        }

        ESP_LOGW(TAG,
                 "event NET_DOWN type=%d active=%s eth=%d wifi=%d ppp=%d",
                 (int)st->type,
                 uplink_name(s_active_uplink),
                 s_eth_has_ip ? 1 : 0,
                 s_wifi_has_ip ? 1 : 0,
                 s_ppp_has_ip ? 1 : 0);
        break;
    }

    case APP_EVENT_MQTT_CONNECTED:
        ESP_LOGI(TAG, "event MQTT_CONNECTED");
        clear_server_watchdog();
        break;

    case APP_EVENT_MQTT_DISCONNECTED:
        ESP_LOGW(TAG, "event MQTT_DISCONNECTED");
        break;

    case APP_EVENT_PROVISION_START:
        ESP_LOGW(TAG, "event PROVISION_START");
        s_provision_active = true;
        app_recovery_set_busy(APP_RECOVERY_BUSY_PROVISION, true);
        break;

    case APP_EVENT_WIFI_CONFIG_SAVED:
        ESP_LOGI(TAG, "event WIFI_CONFIG_SAVED");
        s_wifi_config_ready = true;
        s_provision_active = false;
        app_recovery_set_busy(APP_RECOVERY_BUSY_PROVISION, false);

        s_wifi_no_ap_found = false;
        s_wifi_auth_fail = false;
        s_wifi_stuck_since_tick = 0;
        break;

    default:
        break;
    }
}

static bool wifi_reason_is_auth_fail(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *data)
{
    (void)arg;
    (void)base;

    if (event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    wifi_event_sta_disconnected_t *e =
        (wifi_event_sta_disconnected_t *)data;

    if (!e) {
        return;
    }

    ESP_LOGW(TAG,
             "wifi disconnected reason=%d",
             (int)e->reason);

    if (e->reason == WIFI_REASON_NO_AP_FOUND) {
        s_wifi_no_ap_found = true;
        s_wifi_auth_fail = false;

        ESP_LOGW(TAG,
                 "wifi router/AP not found -> do not count as router ON stuck");
    } else if (wifi_reason_is_auth_fail(e->reason)) {
        s_wifi_auth_fail = true;
        s_wifi_no_ap_found = false;

        ESP_LOGW(TAG,
                 "wifi auth fail -> do not reboot loop, wait config");
    } else {
        /*
         * Các reason khác có thể coi là Wi-Fi stuck nếu kéo dài.
         */
        s_wifi_no_ap_found = false;
        s_wifi_auth_fail = false;
    }
}

static void load_initial_config_state(void)
{
    runtime_config_t cfg = {0};

    if (runtime_config_load(&cfg) == ESP_OK &&
        cfg.wifi_enabled &&
        cfg.wifi_ssid[0] != '\0') {

        s_wifi_config_ready = true;
    } else {
        s_wifi_config_ready = false;
    }

    ESP_LOGI(TAG,
             "initial wifi_config_ready=%d",
             s_wifi_config_ready ? 1 : 0);
}

static void print_last_reset_reason(void)
{
    if (s_reset_marker.magic != APP_RECOVERY_RESET_MAGIC) {
        return;
    }

    ESP_LOGW(TAG,
             "last recovery reset reason=%s lost_ms=%lu active=%lu eth=%lu wifi=%lu ppp=%lu",
             reboot_reason_name((app_recovery_reboot_reason_t)s_reset_marker.reason),
             (unsigned long)s_reset_marker.lost_ms,
             (unsigned long)s_reset_marker.active_uplink,
             (unsigned long)s_reset_marker.eth_has_ip,
             (unsigned long)s_reset_marker.wifi_has_ip,
             (unsigned long)s_reset_marker.ppp_has_ip);

    /*
     * Clear để lần sau không in lại mãi.
     */
    memset(&s_reset_marker, 0, sizeof(s_reset_marker));
}

static bool app_recovery_in_boot_grace(TickType_t now)
{
    if (s_boot_grace_start_tick == 0) {
        return false;
    }

    uint32_t elapsed_ms =
        pdTICKS_TO_MS(now - s_boot_grace_start_tick);

    if (elapsed_ms < APP_RECOVERY_BOOT_GRACE_MS) {
        if (!s_boot_grace_logged) {
            s_boot_grace_logged = true;

            ESP_LOGW(TAG,
                     "boot grace active %lu/%lu ms, watchdog delayed",
                     (unsigned long)elapsed_ms,
                     (unsigned long)APP_RECOVERY_BOOT_GRACE_MS);
        }

        return true;
    }

    return false;
}

static void app_recovery_task(void *arg)
{
    (void)arg;

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        /*
         * Sau khi boot, cho Ethernet/WiFi/PPP/MQTT/modem co thoi gian khoi tao.
         * Trong thoi gian nay khong tinh mat server / wifi stuck.
         */
        if (app_recovery_in_boot_grace(now)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        handle_server_watchdog(now);
        handle_wifi_stuck_watchdog(now);
        handle_pending_reboot();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_recovery_init(void)
{
    load_initial_config_state();
    print_last_reset_reason();

    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS,
                                               ESP_EVENT_ANY_ID,
                                               app_event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               WIFI_EVENT_STA_DISCONNECTED,
                                               wifi_event_handler,
                                               NULL));

    return ESP_OK;
}

esp_err_t app_recovery_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_started = true;

     s_started = true;

    s_boot_grace_start_tick = xTaskGetTickCount();
    s_boot_grace_logged = false;
    
    BaseType_t ok = xTaskCreate(app_recovery_task,
                                "app_recovery",
                                APP_RECOVERY_TASK_STACK,
                                NULL,
                                APP_RECOVERY_TASK_PRIO,
                                &s_task);

    if (ok != pdPASS) {
        s_task = NULL;
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "app recovery started");

    return ESP_OK;
}