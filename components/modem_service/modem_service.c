#include "modem_service.h"
#include "app_config.h"
#include "app_events.h"
#include "board.h"
#include "diag_service.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "modem_service";
static char s_sms_resp_buf[2048];
static char s_sms_send_buf[512];

#define MODEM_IP_READY_BIT BIT0

#ifndef APP_MODEM_READY_RETRIES
#define APP_MODEM_READY_RETRIES 10
#endif

#ifndef APP_MODEM_READY_RETRY_MS
#define APP_MODEM_READY_RETRY_MS 1000
#endif

#ifndef APP_MODEM_SYNC_RETRIES
#define APP_MODEM_SYNC_RETRIES 8
#endif

#ifndef APP_MODEM_SYNC_RETRY_MS
#define APP_MODEM_SYNC_RETRY_MS 500
#endif

#ifndef APP_MODEM_HARD_RECOVER_FAILS
#define APP_MODEM_HARD_RECOVER_FAILS 3
#endif

#ifndef APP_MODEM_EARLY_PPP_LOST_MS
#define APP_MODEM_EARLY_PPP_LOST_MS 15000
#endif

static modem_service_config_t s_cfg;
static esp_modem_dce_t *s_dce;
static esp_netif_t *s_ppp_netif;
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_cs_op_lock;
static modem_state_t s_state = MODEM_STATE_OFF;

static volatile bool s_manual_ppp_suspend;
static volatile bool s_cs_session_active;
static bool s_cs_session_had_ppp = false;
static uint32_t s_sync_fail_streak;
static uint32_t s_early_ppp_lost_count;
static TickType_t s_ppp_up_tick;

static esp_err_t modem_wait_for_at_ready(int timeout_ms);
static volatile bool s_sim_card_ready = false;
static TickType_t s_cs_session_start_tick = 0;
static uint8_t s_cs_at_sync_fail_count = 0;

static esp_err_t modem_poll_unread_sms_command_mode(char *number,
                                                    size_t number_len,
                                                    char *text,
                                                    size_t text_len,
                                                    bool *found);
static esp_err_t modem_send_sms_command_mode(const char *number, const char *text);
                                                    static void modem_escape_data_mode_uart(uart_port_t port)
{
    uart_wait_tx_done(port, pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(1200));
    uart_write_bytes(port, "+++", 3);
    uart_wait_tx_done(port, pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(1200));
}

static esp_err_t modem_at_cmd(const char *cmd, char *out, size_t out_sz, uint32_t timeout_ms)
{
    if (!s_dce || !cmd) {
        return ESP_ERR_INVALID_STATE;
    }

    if (out && out_sz > 0) {
        memset(out, 0, out_sz);
    }

    esp_err_t err = esp_modem_at(s_dce, cmd, out, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AT failed cmd=%s err=%s", cmd, esp_err_to_name(err));
        return err;
    }

    if (out && out_sz > 0) {
        ESP_LOGI(TAG, "AT %s => %s", cmd, out);
    } else {
        ESP_LOGI(TAG, "AT %s => OK", cmd);
    }
    return ESP_OK;
}

static bool str_has(const char *s, const char *needle)
{
    return s && needle && strstr(s, needle) != NULL;
}

static esp_err_t modem_wait_ready_for_ppp(void)
{
    char buf[128];

    for (int i = 0; i < APP_MODEM_READY_RETRIES; ++i) {
        bool sim_ready = false;
        bool reg_ready = false;
        bool signal_ok = false;

        (void)modem_at_cmd("AT", buf, sizeof(buf), 1000);

        if (modem_at_cmd("AT+CPIN?", buf, sizeof(buf), 3000) == ESP_OK) {
            sim_ready = str_has(buf, "READY");
            modem_service_set_sim_ready(sim_ready);
        } else {
            sim_ready = false;
            modem_service_set_sim_ready(false);
        }
        if (modem_at_cmd("AT+CSQ", buf, sizeof(buf), 2000) == ESP_OK) {
            int rssi = -1, ber = -1;
            if (sscanf(buf, "%*[^:]: %d,%d", &rssi, &ber) == 2) {
                signal_ok = (rssi >= 10 && rssi != 99);
                ESP_LOGI(TAG, "CSQ rssi=%d ber=%d signal_ok=%d", rssi, ber, signal_ok);
            }
        }

        if (modem_at_cmd("AT+CEREG?", buf, sizeof(buf), 3000) == ESP_OK) {
            int n = 0, stat = 0;
            if (sscanf(buf, "%*[^:]: %d,%d", &n, &stat) == 2) {
                reg_ready = (stat == 1 || stat == 5);
                ESP_LOGI(TAG, "CEREG stat=%d reg_ready=%d", stat, reg_ready);
            }
        }

        if (!reg_ready && modem_at_cmd("AT+CREG?", buf, sizeof(buf), 3000) == ESP_OK) {
            int n = 0, stat = 0;
            if (sscanf(buf, "%*[^:]: %d,%d", &n, &stat) == 2) {
                reg_ready = (stat == 1 || stat == 5);
                ESP_LOGI(TAG, "CREG stat=%d reg_ready=%d", stat, reg_ready);
            }
        }

        if (sim_ready && reg_ready && signal_ok) {
            ESP_LOGI(TAG, "modem ready for PPP");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "modem not ready sim=%d reg=%d sig=%d try=%d/%d",
                 sim_ready, reg_ready, signal_ok, i + 1, APP_MODEM_READY_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(APP_MODEM_READY_RETRY_MS));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t modem_sync_with_recover(void)
{
    for (int attempt = 0; attempt < APP_MODEM_SYNC_RETRIES; ++attempt) {
        esp_err_t err = esp_modem_sync(s_dce);
        if (err == ESP_OK) {
            s_sync_fail_streak = 0;
            ESP_LOGI(TAG, "sync ok");
            return ESP_OK;
        }

        diag_inc_sync_fail();
        ESP_LOGW(TAG, "sync timeout attempt=%d", attempt + 1);
        modem_escape_data_mode_uart(s_cfg.uart_port);
        vTaskDelay(pdMS_TO_TICKS(APP_MODEM_SYNC_RETRY_MS));
    }

    s_sync_fail_streak++;
    ESP_LOGW(TAG, "sync failed streak=%lu", (unsigned long)s_sync_fail_streak);

    if (s_sync_fail_streak >= APP_MODEM_HARD_RECOVER_FAILS) {
        ESP_LOGW(TAG, "sync failed too many times -> power cycle modem");
        s_sync_fail_streak = 0;

        diag_inc_modem_power_cycle();

        esp_err_t reset_err = board_modem_power_cycle();
        if (reset_err != ESP_OK) {
            ESP_LOGE(TAG,
                    "modem power cycle failed during sync recover: %s",
                    esp_err_to_name(reset_err));
        } else {
            ESP_LOGW(TAG, "modem power cycle done during sync recover");
        }

        vTaskDelay(pdMS_TO_TICKS(8000));

        s_state = MODEM_STATE_INIT;
        modem_service_set_sim_ready(false);
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t modem_wait_for_at_ready(int timeout_ms)
{
    int64_t start_ms = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start_ms) < timeout_ms) {
        esp_err_t err = modem_sync_with_recover();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "modem AT ready");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGE(TAG, "wait for AT ready timeout");
    return ESP_ERR_TIMEOUT;
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == IP_EVENT && id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP GOT IP: " IPSTR, IP2STR(&e->ip_info.ip));

        xEventGroupSetBits(s_ev, MODEM_IP_READY_BIT);
        s_state = MODEM_STATE_RUNNING;
        modem_service_set_sim_ready(true);
        s_ppp_up_tick = xTaskGetTickCount();
        s_early_ppp_lost_count = 0;

        app_net_status_t st = {
            .type = APP_NET_PPP,
            .has_ip = true
        };
        esp_event_post(APP_EVENTS, APP_EVENT_NET_UP, &st, sizeof(st), portMAX_DELAY);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_PPP_LOST_IP) {
        TickType_t now = xTaskGetTickCount();
        ESP_LOGW(TAG, "PPP LOST IP%s", s_manual_ppp_suspend ? " (manual suspend)" : "");

        xEventGroupClearBits(s_ev, MODEM_IP_READY_BIT);
        s_state = s_manual_ppp_suspend ? MODEM_STATE_OFF : MODEM_STATE_RECOVERING;

        if (s_ppp_up_tick != 0 &&
            (now - s_ppp_up_tick) < pdMS_TO_TICKS(APP_MODEM_EARLY_PPP_LOST_MS)) {
            s_early_ppp_lost_count++;
            ESP_LOGW(TAG, "early PPP lost count=%lu", (unsigned long)s_early_ppp_lost_count);
        } else {
            s_early_ppp_lost_count = 0;
        }

        s_ppp_up_tick = 0;

        if (s_early_ppp_lost_count >= 2) {
            ESP_LOGW(TAG, "PPP lost too early repeatedly -> power cycle modem");
            s_early_ppp_lost_count = 0;
            diag_inc_modem_power_cycle();
            ESP_ERROR_CHECK(board_modem_power_cycle());
            vTaskDelay(pdMS_TO_TICKS(8000));
        }

        app_net_status_t st = {
            .type = APP_NET_PPP,
            .has_ip = false
        };
        esp_event_post(APP_EVENTS, APP_EVENT_NET_DOWN, &st, sizeof(st), portMAX_DELAY);
        return;
    }
}

static esp_err_t create_dce(void)
{
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = s_cfg.uart_port;
    dte_cfg.uart_config.tx_io_num = s_cfg.tx_pin;
    dte_cfg.uart_config.rx_io_num = s_cfg.rx_pin;
    dte_cfg.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
    dte_cfg.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_cfg.uart_config.baud_rate = s_cfg.baudrate;
    dte_cfg.uart_config.rx_buffer_size = 4096;
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.uart_config.event_queue_size = 30;
    dte_cfg.task_stack_size = 4096;
    dte_cfg.task_priority = 10;
    dte_cfg.dte_buffer_size = 1024;

    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(s_cfg.apn);
    s_dce = esp_modem_new(&dte_cfg, &dce_cfg, s_ppp_netif);
    return s_dce ? ESP_OK : ESP_FAIL;
}

esp_err_t modem_service_init(const modem_service_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->apn, ESP_ERR_INVALID_ARG, TAG, "bad cfg");
    s_cfg = *cfg;

    s_ev = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ev != NULL, ESP_ERR_NO_MEM, TAG, "event group fail");

    s_cs_op_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_cs_op_lock != NULL, ESP_ERR_NO_MEM, TAG, "cs op lock fail");

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&ppp_cfg);
    ESP_RETURN_ON_FALSE(s_ppp_netif != NULL, ESP_FAIL, TAG, "ppp netif fail");

    s_sync_fail_streak = 0;
    s_early_ppp_lost_count = 0;
    s_ppp_up_tick = 0;
    s_manual_ppp_suspend = false;
    s_cs_session_active = false;

    s_state = MODEM_STATE_INIT;
    return create_dce();
}

esp_err_t modem_service_start(void)
{
    char tmp[128] = {0};

    if (!s_dce) {
        ESP_LOGE(TAG, "DCE not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MODEM_STATE_WAIT_IP || s_state == MODEM_STATE_RUNNING) {
        ESP_LOGW(TAG, "PPP already active state=%d", (int)s_state);
        return ESP_OK;
    }

    xEventGroupClearBits(s_ev, MODEM_IP_READY_BIT);
    s_ppp_up_tick = 0;

    s_state = MODEM_STATE_SYNC;
    ESP_RETURN_ON_ERROR(modem_sync_with_recover(), TAG, "sync fail");

    (void)modem_at_cmd("AT+CSCLK=0", tmp, sizeof(tmp), 1000);

    if (s_cfg.apn && s_cfg.apn[0]) {
        char apn_cmd[96];
        snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", s_cfg.apn);
        (void)modem_at_cmd(apn_cmd, tmp, sizeof(tmp), 1500);
    }

    ESP_RETURN_ON_ERROR(modem_wait_ready_for_ppp(), TAG, "modem not ready for PPP");

    s_state = MODEM_STATE_DATA;
    ESP_RETURN_ON_ERROR(esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA), TAG, "set data fail");
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_state = MODEM_STATE_WAIT_IP;
    ESP_LOGI(TAG, "PPP started, waiting IP");
    return ESP_OK;
}

esp_err_t modem_service_restart_ppp(void)
{
    diag_inc_ppp_restart();

    xEventGroupClearBits(s_ev, MODEM_IP_READY_BIT);
    s_ppp_up_tick = 0;
    s_state = MODEM_STATE_RECOVERING;

    esp_err_t err = modem_wait_ready_for_ppp();
    if (err != ESP_OK) {
        return err;
    }

    s_state = MODEM_STATE_DATA;
    err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err == ESP_OK) {
        s_state = MODEM_STATE_WAIT_IP;
    }
    return err;
}

esp_err_t modem_service_power_cycle_and_restart(void)
{
    diag_inc_modem_power_cycle();

    xEventGroupClearBits(s_ev, MODEM_IP_READY_BIT);
    s_ppp_up_tick = 0;
    s_state = MODEM_STATE_RECOVERING;

    ESP_ERROR_CHECK(board_modem_power_cycle());
    vTaskDelay(pdMS_TO_TICKS(8000));

    return modem_service_start();
}

esp_err_t modem_service_stop_ppp(void)
{
    xEventGroupClearBits(s_ev, MODEM_IP_READY_BIT);
    s_ppp_up_tick = 0;

    if (!s_dce) {
        s_state = MODEM_STATE_OFF;
        return ESP_OK;
    }

    s_state = MODEM_STATE_OFF;
    modem_escape_data_mode_uart(s_cfg.uart_port);
    return esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
}

bool modem_service_is_ip_ready(void)
{
    return s_ev && ((xEventGroupGetBits(s_ev) & MODEM_IP_READY_BIT) != 0);
}

modem_state_t modem_service_get_state(void)
{
    return s_state;
}

bool modem_service_should_auto_restart_ppp(void)
{
    return !s_manual_ppp_suspend;
}

bool modem_service_is_cs_session_active(void)
{
    return s_cs_session_active;
}

static esp_err_t modem_service_begin_cs_session(const char *name)
{
    const char *session_name = name ? name : "cs";

    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: modem busy timeout", session_name);
        return ESP_ERR_TIMEOUT;
    }

    s_cs_session_active = true;
    s_cs_session_had_ppp = false;
    s_cs_session_start_tick = xTaskGetTickCount();

    /*
     * Chi stop PPP neu truoc do modem that su dang chay PPP/DATA.
     */
    s_cs_session_had_ppp =
        modem_service_is_ip_ready() ||
        (s_state == MODEM_STATE_DATA) ||
        (s_state == MODEM_STATE_WAIT_IP) ||
        (s_state == MODEM_STATE_RUNNING);

    bool need_restart_ppp = s_cs_session_had_ppp;

    if (s_cs_session_had_ppp) {
        s_manual_ppp_suspend = true;

        ESP_LOGW(TAG, "%s: stop PPP before CS service", session_name);

        esp_err_t stop_err = modem_service_stop_ppp();

        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "%s: stop PPP failed: %s, try continue CS sync",
                     session_name,
                     esp_err_to_name(stop_err));

            /*
             * Khong return ngay.
             * Co truong hop PPP dang mat IP / dang closing,
             * stop PPP tra loi, nhung modem van co the quay lai AT.
             */
        }

        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        /*
         * Dang dung Ethernet/WiFi hoac modem idle command mode:
         * khong stop PPP, khong manual suspend.
         */
        s_manual_ppp_suspend = false;

        ESP_LOGI(TAG, "%s: no PPP active, use command mode", session_name);
    }
    esp_err_t err = modem_wait_for_at_ready(15000);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: sync before CS service failed: %s",
                session_name,
                esp_err_to_name(err));

        s_cs_at_sync_fail_count++;

        ESP_LOGE(TAG,
                "%s: CS AT sync fail count=%u",
                session_name,
                (unsigned)s_cs_at_sync_fail_count);

        /*
        * Clear CS session/busy truoc, khong de app bi ket modem busy.
        */
        s_manual_ppp_suspend = false;
        s_cs_session_active = false;
        s_cs_session_had_ppp = false;
        s_cs_session_start_tick = 0;

        xSemaphoreGive(s_cs_op_lock);

        /*
        * Neu truoc do dang co PPP, thu restart PPP lai.
        */
        if (need_restart_ppp) {
            ESP_LOGW(TAG, "%s: AT sync failed -> try restart PPP", session_name);

            esp_err_t restart_err = modem_service_restart_ppp();
            if (restart_err != ESP_OK) {
                ESP_LOGE(TAG, "%s: restart PPP after AT sync failed: %s",
                        session_name,
                        esp_err_to_name(restart_err));
            }
        } else {
            /*
            * Dang Ethernet/WiFi, khong co PPP de restart.
            * Neu modem lien tuc khong tra loi AT thi power cycle modem.
            */
            if (s_cs_at_sync_fail_count >= 2) {
                ESP_LOGE(TAG,
                        "%s: AT sync failed too many times without PPP -> power cycle modem",
                        session_name);

                s_cs_at_sync_fail_count = 0;

                diag_inc_modem_power_cycle();

                esp_err_t reset_err = board_modem_power_cycle();
                if (reset_err != ESP_OK) {
                    ESP_LOGE(TAG,
                            "%s: modem power cycle failed: %s",
                            session_name,
                            esp_err_to_name(reset_err));
                } else {
                    ESP_LOGW(TAG, "%s: modem power cycle done", session_name);
                }

                vTaskDelay(pdMS_TO_TICKS(8000));

                s_state = MODEM_STATE_INIT;
                modem_service_set_sim_ready(false);
            }
        }

        return err;
    }

    s_cs_at_sync_fail_count = 0;
    return ESP_OK;
    // esp_err_t err = modem_wait_for_at_ready(15000);

    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "%s: sync before CS service failed: %s",
    //              session_name,
    //              esp_err_to_name(err));

    //     /*
    //      * Clear CS session/busy truoc, khong de app bi ket modem busy.
    //      */
    //     s_manual_ppp_suspend = false;
    //     s_cs_session_active = false;
    //     s_cs_session_had_ppp = false;
    //     s_cs_session_start_tick = 0;

    //     xSemaphoreGive(s_cs_op_lock);

    //     /*
    //      * Neu truoc do dang co PPP, thu restart PPP lai.
    //      */
    //     if (need_restart_ppp) {
    //         ESP_LOGW(TAG, "%s: AT sync failed -> try restart PPP", session_name);

    //         esp_err_t restart_err = modem_service_restart_ppp();
    //         if (restart_err != ESP_OK) {
    //             ESP_LOGE(TAG, "%s: restart PPP after AT sync failed: %s",
    //                      session_name,
    //                      esp_err_to_name(restart_err));
    //         }
    //     }

    //     return err;
    // }

    // return ESP_OK;
}

static esp_err_t modem_service_end_cs_session(const char *name, esp_err_t op_err)
{
    const char *session_name = name ? name : "cs";
    esp_err_t result = op_err;

    /*
     * Lưu lại việc trước đó có PPP hay không.
     * Sau đó phải clear busy TRƯỚC khi restart PPP.
     */
    bool need_restart_ppp = s_cs_session_had_ppp;

    /*
     * Quan trọng:
     * Không được giữ s_cs_session_active trong lúc restart PPP.
     * Nếu SIM không có 4G / hết data, restart PPP có thể fail lâu,
     * làm TT/alarm SMS bị chặn mãi.
     */
    s_manual_ppp_suspend = false;
    s_cs_session_active = false;
    s_cs_session_had_ppp = false;
    s_cs_session_start_tick = 0;

    xSemaphoreGive(s_cs_op_lock);

    ESP_LOGW(TAG, "%s: CS session ended, modem busy cleared", session_name);

    if (need_restart_ppp) {
        ESP_LOGW(TAG, "%s: restart PPP after CS service", session_name);

        esp_err_t restart_err = modem_service_restart_ppp();

        if (restart_err != ESP_OK) {
            ESP_LOGE(TAG, "%s: restart PPP failed: %s",
                     session_name, esp_err_to_name(restart_err));

        }
    } else {
        ESP_LOGI(TAG, "%s: no PPP before CS service, keep command mode", session_name);
    }

    return result;
}

static esp_err_t modem_send_sms_command_mode(const char *number, const char *text)
{
    char *resp = s_sms_send_buf;
    char cmd[96];
    esp_err_t err = ESP_FAIL;

    // if (!s_dce || !number || !text) {
    //     return ESP_ERR_INVALID_ARG;
    // }
    if (!s_dce || !number || !text || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(resp, 0, sizeof(s_sms_send_buf));

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)modem_at_cmd("ATE0", resp, sizeof(s_sms_send_buf), 1000);

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(resp, 0, sizeof(s_sms_send_buf));
    err = modem_at_cmd("AT+CMGF=1", resp, sizeof(s_sms_send_buf), 3000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set text mode failed: %s", esp_err_to_name(err));
        goto out_sms;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r", number);
    ESP_LOGI(TAG, "AT+CMGS to %s", number);

    uart_write_bytes(s_cfg.uart_port, cmd, strlen(cmd));
    uart_wait_tx_done(s_cfg.uart_port, pdMS_TO_TICKS(1000));

    memset(resp, 0, sizeof(s_sms_send_buf));
    size_t used = 0;
    int idle_rounds = 0;

    while (used < sizeof(s_sms_send_buf) - 1 && idle_rounds < 10) {
        int len = uart_read_bytes(
            s_cfg.uart_port,
            (uint8_t *)&resp[used],
            sizeof(s_sms_send_buf) - 1 - used,
            pdMS_TO_TICKS(200)
        );

        if (len > 0) {
            used += (size_t)len;
            resp[used] = 0;
            if (strchr(resp, '>') != NULL) {
                break;
            }
            idle_rounds = 0;
        } else {
            idle_rounds++;
        }
    }

    ESP_LOGI(TAG, "CMGS prompt resp: [%s]", resp);

    if (strchr(resp, '>') == NULL) {
        ESP_LOGE(TAG, "CMGS prompt timeout/invalid");
        err = ESP_ERR_TIMEOUT;
        goto out_sms;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(20));

    uart_write_bytes(s_cfg.uart_port, text, strlen(text));
    {
        const char ctrlz = 0x1A;
        uart_write_bytes(s_cfg.uart_port, &ctrlz, 1);
    }
    uart_wait_tx_done(s_cfg.uart_port, pdMS_TO_TICKS(2000));

    memset(resp, 0, sizeof(s_sms_send_buf));
    used = 0;
    idle_rounds = 0;

    while (used < sizeof(s_sms_send_buf) - 1 && idle_rounds < 20) {
        int len = uart_read_bytes(
            s_cfg.uart_port,
            (uint8_t *)&resp[used],
            sizeof(s_sms_send_buf) - 1 - used,
            pdMS_TO_TICKS(500)
        );

        if (len > 0) {
            used += (size_t)len;
            resp[used] = 0;

            if (strstr(resp, "+CMGS:") != NULL && strstr(resp, "OK") != NULL) {
                break;
            }
            if (strstr(resp, "ERROR") != NULL) {
                break;
            }
            idle_rounds = 0;
        } else {
            idle_rounds++;
        }
    }

    ESP_LOGI(TAG, "sms resp: [%s]", resp);

    if (strstr(resp, "+CMGS:") == NULL && strstr(resp, "OK") == NULL) {
        ESP_LOGE(TAG, "SMS send failed");
        err = ESP_FAIL;
        goto out_sms;
    }

    err = ESP_OK;

out_sms:
    return err;
}

esp_err_t modem_service_send_sms(const char *number, const char *text)
{
    esp_err_t err;

    if (!s_dce || !number || !text || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = modem_service_begin_cs_session("sms");
    if (err != ESP_OK) {
        return err;
    }

    err = modem_send_sms_command_mode(number, text);

    return modem_service_end_cs_session("sms", err);
}

// esp_err_t modem_service_start_call(const char *number)
// {
//     char resp[512] = {0};
//     char cmd[96];

//     if (!s_dce || !number || !number[0]) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     if (!s_cs_op_lock || xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
//         return ESP_ERR_TIMEOUT;
//     }

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     (void)modem_at_cmd("ATE0", resp, sizeof(resp), 1000);

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     snprintf(cmd, sizeof(cmd), "ATD%s;", number);
//     memset(resp, 0, sizeof(resp));

//     esp_err_t err = modem_at_cmd(cmd, resp, sizeof(resp), 3000);
//     if (err == ESP_OK) {
//         ESP_LOGI(TAG, "voice call dialing to %s", number);
//     } else {
//         ESP_LOGE(TAG, "dial failed: %s", esp_err_to_name(err));
//     }

//     xSemaphoreGive(s_cs_op_lock);
//     return err;
// }

static esp_err_t modem_start_call_command_mode(const char *number)
{
    char resp[512] = {0};
    char cmd[96];

    if (!s_dce || !number || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)modem_at_cmd("ATE0", resp, sizeof(resp), 1000);

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    snprintf(cmd, sizeof(cmd), "ATD%s;", number);

    memset(resp, 0, sizeof(resp));
    esp_err_t err = modem_at_cmd(cmd, resp, sizeof(resp), 3000);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "voice call dialing to %s", number);
    } else {
        ESP_LOGE(TAG, "dial failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t modem_service_start_call(const char *number)
{
    esp_err_t err;

    if (!s_dce || !number || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = modem_service_begin_cs_session("start_call");
    if (err != ESP_OK) {
        return err;
    }

    err = modem_start_call_command_mode(number);

    return modem_service_end_cs_session("start_call", err);
}

// esp_err_t modem_service_hangup(void)
// {
//     char resp[256] = {0};

//     if (!s_dce) {
//         return ESP_ERR_INVALID_STATE;
//     }

//     if (!s_cs_op_lock || xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
//         return ESP_ERR_TIMEOUT;
//     }

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     esp_err_t err = modem_at_cmd("ATH", resp, sizeof(resp), 3000);
//     if (err != ESP_OK) {
//         ESP_LOGW(TAG, "hangup with ATH failed, try AT+CHUP");
//         memset(resp, 0, sizeof(resp));
//         err = modem_at_cmd("AT+CHUP", resp, sizeof(resp), 3000);
//     }

//     if (err == ESP_OK) {
//         ESP_LOGI(TAG, "voice call ended");
//     } else {
//         ESP_LOGE(TAG, "hangup failed");
//     }

//     xSemaphoreGive(s_cs_op_lock);
//     return err;
// }

static esp_err_t modem_hangup_command_mode(void)
{
    char resp[256] = {0};

    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = modem_at_cmd("ATH", resp, sizeof(resp), 3000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hangup with ATH failed, try AT+CHUP");

        memset(resp, 0, sizeof(resp));
        err = modem_at_cmd("AT+CHUP", resp, sizeof(resp), 3000);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "voice call ended");
    } else {
        ESP_LOGE(TAG, "hangup failed");
    }

    return err;
}

esp_err_t modem_service_hangup(void)
{
    esp_err_t err;

    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }

    err = modem_service_begin_cs_session("hangup");
    if (err != ESP_OK) {
        return err;
    }

    err = modem_hangup_command_mode();

    return modem_service_end_cs_session("hangup", err);
}

// esp_err_t modem_service_make_call(const char *number, uint32_t duration_ms)
// {
//     char resp[512] = {0};
//     char cmd[96];
//     esp_err_t err = ESP_FAIL;

//     if (!s_dce || !number || !number[0]) {
//         return ESP_ERR_INVALID_ARG;
//     }

//     if (duration_ms < 5000) {
//         duration_ms = 5000;
//     }

//     if (!s_cs_op_lock || xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
//         return ESP_ERR_TIMEOUT;
//     }

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     memset(resp, 0, sizeof(resp));
//     (void)modem_at_cmd("ATE0", resp, sizeof(resp), 1000);

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     snprintf(cmd, sizeof(cmd), "ATD%s;", number);
//     memset(resp, 0, sizeof(resp));
//     err = modem_at_cmd(cmd, resp, sizeof(resp), 10000);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "dial failed: %s", esp_err_to_name(err));
//         goto out_call;
//     }

//     ESP_LOGI(TAG, "voice call dialing to %s", number);

//     vTaskDelay(pdMS_TO_TICKS(duration_ms));

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     memset(resp, 0, sizeof(resp));
//     err = modem_at_cmd("ATH", resp, sizeof(resp), 5000);
//     if (err != ESP_OK) {
//         ESP_LOGW(TAG, "hangup with ATH failed, try AT+CHUP");
//         memset(resp, 0, sizeof(resp));
//         err = modem_at_cmd("AT+CHUP", resp, sizeof(resp), 5000);
//         if (err != ESP_OK) {
//             ESP_LOGE(TAG, "hangup failed");
//             goto out_call;
//         }
//     }

//     ESP_LOGI(TAG, "voice call ended");
//     err = ESP_OK;

// out_call:
//     if (s_cs_op_lock) {
//         xSemaphoreGive(s_cs_op_lock);
//     }
//     return err;
// }

static esp_err_t modem_make_call_command_mode(const char *number, uint32_t duration_ms)
{
    char resp[512] = {0};
    char cmd[96];

    if (!s_dce || !number || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (duration_ms < 5000) {
        duration_ms = 5000;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)modem_at_cmd("ATE0", resp, sizeof(resp), 1000);

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    snprintf(cmd, sizeof(cmd), "ATD%s;", number);

    memset(resp, 0, sizeof(resp));
    esp_err_t err = modem_at_cmd(cmd, resp, sizeof(resp), 10000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dial failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "voice call dialing to %s", number);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(resp, 0, sizeof(resp));
    err = modem_at_cmd("ATH", resp, sizeof(resp), 5000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hangup with ATH failed, try AT+CHUP");

        memset(resp, 0, sizeof(resp));
        err = modem_at_cmd("AT+CHUP", resp, sizeof(resp), 5000);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "voice call ended");
    } else {
        ESP_LOGE(TAG, "hangup failed");
    }

    return err;
}

esp_err_t modem_service_make_call(const char *number, uint32_t duration_ms)
{
    esp_err_t err;

    if (!s_dce || !number || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = modem_service_begin_cs_session("call");
    if (err != ESP_OK) {
        return err;
    }

    err = modem_make_call_command_mode(number, duration_ms);

    return modem_service_end_cs_session("call", err);
}

static void trim_ascii(char *s)
{
    if (!s) return;
    char *start = s;
    while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t' || *start == '"') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n' || end[-1] == '\t' || end[-1] == '"')) end--;
    *end = 0;
    if (start != s) memmove(s, start, end - start + 1);
}

static bool modem_sms_ready(void)
{
    char resp[128] = {0};
    bool sim_ready = false;
    bool reg_ready = false;

    if (!s_dce) {
        modem_service_set_sim_ready(false);
        return false;
    }

    if (modem_at_cmd("AT+CPIN?", resp, sizeof(resp), 3000) == ESP_OK) {
        sim_ready = (strstr(resp, "READY") != NULL);
        modem_service_set_sim_ready(sim_ready);

        ESP_LOGI(TAG, "CPIN resp: %s", resp);

    } else {
        if (s_state == MODEM_STATE_DATA ||
            s_state == MODEM_STATE_WAIT_IP ||
            s_state == MODEM_STATE_RUNNING) {

            ESP_LOGW(TAG, "AT+CPIN? failed in PPP/DATA mode, keep SIM ready state");
        } else {
            modem_service_set_sim_ready(false);
        }

        ESP_LOGW(TAG, "AT+CPIN? failed");
        return false;
    }
    memset(resp, 0, sizeof(resp));
    if (modem_at_cmd("AT+CEREG?", resp, sizeof(resp), 3000) == ESP_OK) {
        int n = 0, stat = 0;
        if (sscanf(resp, "%*[^:]: %d,%d", &n, &stat) == 2) {
            reg_ready = (stat == 1 || stat == 5);
        }
        ESP_LOGI(TAG, "CEREG resp: %s", resp);
    }

    if (!reg_ready) {
        memset(resp, 0, sizeof(resp));
        if (modem_at_cmd("AT+CREG?", resp, sizeof(resp), 3000) == ESP_OK) {
            int n = 0, stat = 0;
            if (sscanf(resp, "%*[^:]: %d,%d", &n, &stat) == 2) {
                reg_ready = (stat == 1 || stat == 5);
            }
            ESP_LOGI(TAG, "CREG resp: %s", resp);
        }
    }

    if (!sim_ready) {
        ESP_LOGW(TAG, "SMS not ready: SIM not ready");
        return false;
    }

    if (!reg_ready) {
        ESP_LOGW(TAG, "SMS not ready: network not registered");
        return false;
    }

    return true;
}

static esp_err_t modem_prepare_sms_storage(char *resp, size_t resp_sz)
{
    esp_err_t err;

    if (!s_dce || !resp || resp_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "AT+CPMS SM failed, fallback to MT");

    memset(resp, 0, resp_sz);
    err = modem_at_cmd("AT+CPMS=\"MT\",\"MT\",\"MT\"", resp, resp_sz, 3000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SMS storage = MT");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "AT+CPMS MT failed too");
    return err;
}

static esp_err_t modem_set_sms_storage(const char *storage, char *resp, size_t resp_sz)
{
    char cmd[64];

    if (!storage || !resp || resp_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(cmd, sizeof(cmd),
             "AT+CPMS=\"%s\",\"%s\",\"%s\"",
             storage, storage, storage);

    memset(resp, 0, resp_sz);

    esp_err_t err = modem_at_cmd(cmd, resp, resp_sz, 3000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SMS storage set to %s resp=[%s]", storage, resp);
    } else {
        ESP_LOGW(TAG, "SMS storage set to %s failed: %s",
                 storage, esp_err_to_name(err));
    }

    return err;
}

static esp_err_t modem_read_cmgl_all(char *resp, size_t resp_sz)
{
    if (!resp || resp_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(resp, 0, resp_sz);

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    const char *cmd = "AT+CMGL=\"ALL\"\r\n";

    uart_write_bytes(s_cfg.uart_port, cmd, strlen(cmd));
    uart_wait_tx_done(s_cfg.uart_port, pdMS_TO_TICKS(1000));

    size_t used = 0;
    TickType_t start = xTaskGetTickCount();

    while (used < resp_sz - 1) {
        int len = uart_read_bytes(
            s_cfg.uart_port,
            (uint8_t *)&resp[used],
            resp_sz - 1 - used,
            pdMS_TO_TICKS(300)
        );

        if (len > 0) {
            used += (size_t)len;
            resp[used] = '\0';

            if (strstr(resp, "\r\nOK") ||
                strstr(resp, "\nOK") ||
                strstr(resp, "ERROR")) {
                break;
            }
        }

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(12000)) {
            break;
        }
    }

    ESP_LOGW(TAG, "CMGL ALL raw resp: [%s]", resp);

    if (strstr(resp, "ERROR")) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t modem_poll_unread_sms_command_mode(char *number, size_t number_len, char *text, size_t text_len, bool *found)
{
    char *resp = s_sms_resp_buf;
    char cmd[64];
    esp_err_t err = ESP_FAIL;

    if (!number || !text || !found || number_len == 0 || text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *found = false;
    number[0] = 0;
    text[0] = 0;

    // if (!s_cs_op_lock || xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
    //     return ESP_ERR_TIMEOUT;
    // }
    // s_cs_session_active = true;
    // s_cs_session_start_tick = xTaskGetTickCount();

    memset(resp, 0, sizeof(s_sms_resp_buf));

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)modem_at_cmd("ATE0", resp, sizeof(s_sms_resp_buf), 1000);

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (!modem_sms_ready()) {
        ESP_LOGW(TAG, "skip poll unread sms: modem/sim not ready");
        err = ESP_OK;
        goto out;
    }

    memset(resp, 0, sizeof(s_sms_resp_buf));
    err = modem_at_cmd("AT+CMGF=1", resp, sizeof(s_sms_resp_buf), 3000);
    ESP_LOGI(TAG, "CMGF=1 result=%s resp=[%s]", esp_err_to_name(err), resp);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set SMS text mode failed before poll: %s", esp_err_to_name(err));
        err = ESP_OK;
        goto out;
    }

    memset(resp, 0, sizeof(s_sms_resp_buf));
    (void)modem_at_cmd("AT+CPMS?", resp, sizeof(s_sms_resp_buf), 3000);
    ESP_LOGI(TAG, "CPMS? resp: %s", resp);

    /*
    * Đọc SMS ở MT trước.
    * Nếu MT không có SMS thì đọc tiếp SM.
    */
    bool has_sms = false;

    /* 1. Thử MT */
    if (modem_set_sms_storage("MT", resp, sizeof(s_sms_resp_buf)) == ESP_OK) {
        if (modem_read_cmgl_all(resp, sizeof(s_sms_resp_buf)) == ESP_OK &&
            strstr(resp, "+CMGL:") != NULL) {
            has_sms = true;
            ESP_LOGW(TAG, "SMS found in MT");
        }
    }

    /* 2. Nếu MT không có thì thử SM */
    if (!has_sms) {
        ESP_LOGW(TAG, "No SMS in MT, try SM");

        if (modem_set_sms_storage("SM", resp, sizeof(s_sms_resp_buf)) == ESP_OK) {
            if (modem_read_cmgl_all(resp, sizeof(s_sms_resp_buf)) == ESP_OK &&
                strstr(resp, "+CMGL:") != NULL) {
                has_sms = true;
                ESP_LOGW(TAG, "SMS found in SM");
            }
        }
    }

    /* 3. Nếu cả MT và SM đều không có */
    if (!has_sms) {
        ESP_LOGW(TAG, "No SMS found in MT/SM");
        err = ESP_OK;
        goto out;
    }

    char *p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        int index = -1;
        char sms_number[32] = {0};
        char sms_text[256] = {0};

        char *hdr_end = strstr(p, "\n");
        if (!hdr_end) {
            hdr_end = strstr(p, "\r");
        }
        if (!hdr_end) {
            break;
        }

        if (sscanf(p, "+CMGL: %d", &index) != 1 || index < 0) {
            p = hdr_end + 1;
            continue;
        }

        char *q = p;
        int quote_count = 0;
        char *num_start = NULL;
        char *num_end = NULL;
        while (*q && q < hdr_end) {
            if (*q == '"') {
                quote_count++;
                if (quote_count == 3) {
                    num_start = q + 1;
                } else if (quote_count == 4) {
                    num_end = q;
                    break;
                }
            }
            q++;
        }

        if (num_start && num_end && num_end > num_start) {
            size_t n = (size_t)(num_end - num_start);
            if (n >= sizeof(sms_number)) n = sizeof(sms_number) - 1;
            memcpy(sms_number, num_start, n);
            sms_number[n] = 0;
            trim_ascii(sms_number);
        }

        char *msg = hdr_end;
        while (*msg == '\r' || *msg == '\n') msg++;

        char *next_hdr = strstr(msg, "+CMGL:");
        char *ok_pos = strstr(msg, "\r\nOK");
        char *msg_end = NULL;

        if (next_hdr) {
            msg_end = next_hdr;
            while (msg_end > msg && (msg_end[-1] == '\r' || msg_end[-1] == '\n')) {
                msg_end--;
            }
        } else if (ok_pos) {
            msg_end = ok_pos;
            while (msg_end > msg && (msg_end[-1] == '\r' || msg_end[-1] == '\n')) {
                msg_end--;
            }
        } else {
            msg_end = msg + strlen(msg);
            while (msg_end > msg && (msg_end[-1] == '\r' || msg_end[-1] == '\n')) {
                msg_end--;
            }
        }

        if (msg_end > msg) {
            size_t m = (size_t)(msg_end - msg);
            if (m >= sizeof(sms_text)) m = sizeof(sms_text) - 1;
            memcpy(sms_text, msg, m);
            sms_text[m] = 0;
            trim_ascii(sms_text);
        }

        if (sms_number[0] && sms_text[0]) {
            ESP_LOGI(TAG, "SMS received: idx=%d number=[%s] text=[%s]", index, sms_number, sms_text);

            size_t n = strlen(sms_number);
            if (n >= number_len) n = number_len - 1;
            memcpy(number, sms_number, n);
            number[n] = 0;

            size_t m = strlen(sms_text);
            if (m >= text_len) m = text_len - 1;
            memcpy(text, sms_text, m);
            text[m] = 0;

            *found = true;

            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
            memset(resp, 0, sizeof(s_sms_resp_buf));
            (void)modem_at_cmd(cmd, resp, sizeof(s_sms_resp_buf), 3000);

            err = ESP_OK;
            goto out;
        }

        if (next_hdr) {
            p = next_hdr;
        } else {
            break;
        }
    }

    err = ESP_OK;

out:
    // s_manual_ppp_suspend = false;
    // s_cs_session_active = false;
    // s_cs_session_had_ppp = false;
    // s_cs_session_start_tick = 0;

    // if (s_cs_op_lock) {
    //     xSemaphoreGive(s_cs_op_lock);
    // }
    return err;
}

esp_err_t modem_service_poll_unread_sms(char *number,
                                        size_t number_len,
                                        char *text,
                                        size_t text_len,
                                        bool *found)
{
    esp_err_t err;

    if (!number || !text || !found || number_len == 0 || text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    number[0] = '\0';
    text[0] = '\0';
    *found = false;

    err = modem_service_begin_cs_session("poll_sms");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "begin poll_sms session failed: %s", esp_err_to_name(err));
        return err;
    }

    err = modem_poll_unread_sms_command_mode(number,
                                             number_len,
                                             text,
                                             text_len,
                                             found);

    return modem_service_end_cs_session("poll_sms", err);
}

// esp_err_t modem_service_delete_all_sms(void)
// {
//     char resp[128] = {0};

//     if (!s_dce) {
//         return ESP_ERR_INVALID_STATE;
//     }

//     if (!s_cs_op_lock || xSemaphoreTake(s_cs_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
//         return ESP_ERR_TIMEOUT;
//     }

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     (void)modem_at_cmd("ATE0", resp, sizeof(resp), 1000);

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     memset(resp, 0, sizeof(resp));
//     (void)modem_at_cmd("AT+CMGF=1", resp, sizeof(resp), 3000);

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     (void)modem_prepare_sms_storage(resp, sizeof(resp));

//     uart_flush_input(s_cfg.uart_port);
//     vTaskDelay(pdMS_TO_TICKS(50));

//     esp_err_t err = modem_at_cmd("AT+CMGD=1,4", resp, sizeof(resp), 5000);

//     xSemaphoreGive(s_cs_op_lock);
//     return err;
// }

esp_err_t modem_service_delete_all_sms(void)
{
    char resp[256] = {0};
    esp_err_t err = ESP_OK;

    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Dung chung co che CS session:
     * - Neu dang PPP thi stop PPP truoc
     * - Neu Ethernet/WiFi thi dung command mode
     * - Neu AT khong ready thi begin_cs_session tu xu ly recover/reset modem
     */
    err = modem_service_begin_cs_session("delete_sms");
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "delete_sms: begin CS session failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(100));

    /*
     * Tat echo, neu fail thi van co the thu tiep CMGF/CPMS.
     */
    memset(resp, 0, sizeof(resp));
    esp_err_t ate_err = modem_at_cmd("ATE0", resp, sizeof(resp), 1500);
    if (ate_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "delete_sms: ATE0 failed: %s",
                 esp_err_to_name(ate_err));
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(100));

    /*
     * SMS text mode.
     */
    memset(resp, 0, sizeof(resp));
    err = modem_at_cmd("AT+CMGF=1", resp, sizeof(resp), 3000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "delete_sms: AT+CMGF=1 failed: %s",
                 esp_err_to_name(err));

        return modem_service_end_cs_session("delete_sms", err);
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(100));

    /*
     * Chon bo nho SMS.
     * Ham modem_prepare_sms_storage() cua ban da co fallback SM -> MT.
     */
    memset(resp, 0, sizeof(resp));
    err = modem_prepare_sms_storage(resp, sizeof(resp));
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "delete_sms: prepare SMS storage failed: %s",
                 esp_err_to_name(err));

        return modem_service_end_cs_session("delete_sms", err);
    }

    uart_flush_input(s_cfg.uart_port);
    vTaskDelay(pdMS_TO_TICKS(100));

    /*
     * Xoa tat ca SMS.
     * AT+CMGD=1,4: delete all messages from selected storage.
     */
    memset(resp, 0, sizeof(resp));
    err = modem_at_cmd("AT+CMGD=1,4", resp, sizeof(resp), 8000);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "delete_sms: delete all SMS OK");
    } else {
        ESP_LOGE(TAG,
                 "delete_sms: AT+CMGD=1,4 failed: %s resp=[%s]",
                 esp_err_to_name(err),
                 resp);
    }

    return modem_service_end_cs_session("delete_sms", err);
}

esp_err_t modem_service_get_signal(modem_signal_t *sig)
{
    char buf[128] = {0};
    int rssi = -1, ber = -1;
    esp_err_t err;

    if (!sig) {
        return ESP_ERR_INVALID_ARG;
    }

    sig->rssi = -1;
    sig->ber = -1;
    sig->valid = false;

    err = modem_service_begin_cs_session("get_signal");
    if (err != ESP_OK) {
        return err;
    }

    err = modem_at_cmd("AT+CSQ", buf, sizeof(buf), 3000);
    if (err == ESP_OK) {
        if (sscanf(buf, "%*[^:]: %d,%d", &rssi, &ber) == 2) {
            sig->rssi = rssi;
            sig->ber = ber;
            sig->valid = (rssi >= 0 && rssi != 99);
            ESP_LOGI(TAG, "signal rssi=%d ber=%d valid=%d", sig->rssi, sig->ber, sig->valid);
        } else {
            ESP_LOGW(TAG, "parse CSQ failed: %s", buf);
            err = ESP_FAIL;
        }
    }

    return modem_service_end_cs_session("get_signal", err);
}

bool modem_service_is_sim_ready(void)
{
    return s_sim_card_ready;
}

// void modem_service_set_sim_ready(bool ready)
// {
//     s_sim_card_ready = ready;
// }

void modem_service_set_sim_ready(bool ready)
{
    if (s_sim_card_ready != ready) {
        ESP_LOGW(TAG, "SIM ready changed: %d -> %d",
                 s_sim_card_ready ? 1 : 0,
                 ready ? 1 : 0);
    }

    s_sim_card_ready = ready;
}

bool modem_service_is_busy_too_long(uint32_t timeout_ms)
{
    if (!s_cs_session_active || s_cs_session_start_tick == 0) {
        return false;
    }

    return (xTaskGetTickCount() - s_cs_session_start_tick) >= pdMS_TO_TICKS(timeout_ms);
}