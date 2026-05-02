#include "ec800.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define EC800_LINE_MAX        256
#define EC800_RESP_MAX        1024
#define EC800_TASK_STACK      4096
#define EC800_TASK_PRIO       10

static const char *TAG = "ec800";

typedef struct {
    ec800_config_t cfg;
    TaskHandle_t rx_task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t resp_sem;
    ec800_urc_cb_t urc_cb;
    void *urc_ctx;

    char resp_buf[EC800_RESP_MAX];
    size_t resp_len;
    bool waiting_resp;
    bool got_final;
    ec800_result_t final_result;

    ec800_call_state_t call_state;
} ec800_ctx_t;

static ec800_ctx_t g;

static bool line_is_final(const char *line)
{
    return strcmp(line, "OK") == 0 ||
           strcmp(line, "ERROR") == 0 ||
           strstr(line, "+CME ERROR:") == line ||
           strstr(line, "+CMS ERROR:") == line;
}

static bool line_is_urc(const char *line)
{
    return strcmp(line, "RING") == 0 ||
           strcmp(line, "NO CARRIER") == 0 ||
           strcmp(line, "BUSY") == 0 ||
           strcmp(line, "NO ANSWER") == 0 ||
           strstr(line, "+CMTI:") == line ||
           strstr(line, "+CLIP:") == line;
}

static void append_resp_line(const char *line)
{
    size_t len = strlen(line);
    if (g.resp_len + len + 2 >= sizeof(g.resp_buf)) return;
    memcpy(&g.resp_buf[g.resp_len], line, len);
    g.resp_len += len;
    g.resp_buf[g.resp_len++] = '\n';
    g.resp_buf[g.resp_len] = '\0';
}

static void handle_urc(const char *line)
{
    if (strcmp(line, "RING") == 0) {
        g.call_state = EC800_CALL_RINGING;
    } else if (strcmp(line, "NO CARRIER") == 0) {
        g.call_state = EC800_CALL_IDLE;
    }

    if (g.urc_cb) {
        g.urc_cb(line, g.urc_ctx);
    }
}

static void process_line(const char *line)
{
    ESP_LOGI(TAG, "<< %s", line);

    if (g.waiting_resp) {
        if (line_is_final(line)) {
            append_resp_line(line);
            g.final_result = (strcmp(line, "OK") == 0) ? EC800_OK : EC800_ERR_MODEM;
            g.got_final = true;
            xSemaphoreGive(g.resp_sem);
            return;
        }

        if (line_is_urc(line)) {
            handle_urc(line);
            return;
        }

        append_resp_line(line);
        return;
    }

    handle_urc(line);
}

static void ec800_rx_task(void *arg)
{
    uint8_t ch;
    char line[EC800_LINE_MAX];
    size_t pos = 0;

    while (1) {
        int n = uart_read_bytes(g.cfg.uart_num, &ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        if (ch == '\r') continue;

        if (ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                process_line(line);
                pos = 0;
            }
            continue;
        }

        if (pos + 1 < sizeof(line)) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;
        }
    }
}

static ec800_result_t send_cmd_locked(const char *cmd, int timeout_ms, char *out, size_t out_size)
{
    g.resp_len = 0;
    g.resp_buf[0] = '\0';
    g.waiting_resp = true;
    g.got_final = false;
    g.final_result = EC800_ERR_TIMEOUT;

    ESP_LOGI(TAG, ">> %s", cmd);
    uart_write_bytes(g.cfg.uart_num, cmd, strlen(cmd));
    uart_write_bytes(g.cfg.uart_num, "\r\n", 2);

    if (xSemaphoreTake(g.resp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        g.waiting_resp = false;
        return EC800_ERR_TIMEOUT;
    }

    g.waiting_resp = false;

    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", g.resp_buf);
    }
    return g.final_result;
}

static ec800_result_t send_cmd(const char *cmd, int timeout_ms, char *out, size_t out_size)
{
    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return EC800_ERR_BUSY;
    }

    ec800_result_t rc = send_cmd_locked(cmd, timeout_ms, out, out_size);
    xSemaphoreGive(g.lock);
    return rc;
}

esp_err_t ec800_init(const ec800_config_t *cfg, ec800_urc_cb_t urc_cb, void *urc_ctx)
{
    memset(&g, 0, sizeof(g));
    g.cfg = *cfg;
    g.urc_cb = urc_cb;
    g.urc_ctx = urc_ctx;
    g.call_state = EC800_CALL_IDLE;

    uart_config_t uc = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num, cfg->rx_buf_size, cfg->tx_buf_size, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uc));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    g.lock = xSemaphoreCreateMutex();
    g.resp_sem = xSemaphoreCreateBinary();
    if (!g.lock || !g.resp_sem) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ec800_rx_task, "ec800_rx", EC800_TASK_STACK, NULL, EC800_TASK_PRIO, &g.rx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void ec800_deinit(void)
{
    if (g.rx_task) vTaskDelete(g.rx_task);
    if (g.lock) vSemaphoreDelete(g.lock);
    if (g.resp_sem) vSemaphoreDelete(g.resp_sem);
    uart_driver_delete(g.cfg.uart_num);
    memset(&g, 0, sizeof(g));
}

ec800_result_t ec800_start(void)
{
    char resp[EC800_RESP_MAX];

    ec800_result_t rc = send_cmd("AT", 1000, resp, sizeof(resp));
    if (rc != EC800_OK) return rc;

    rc = send_cmd("ATE0", 1000, resp, sizeof(resp));
    if (rc != EC800_OK) return rc;

    rc = send_cmd("AT+CPIN?", 3000, resp, sizeof(resp));
    if (rc != EC800_OK) return rc;

    rc = send_cmd("AT+CMGF=1", 2000, resp, sizeof(resp));
    return rc;
}
// Hàm lấy chất lượng tín hiệu
ec800_result_t ec800_get_signal(int *rssi, int *ber)
{
    char resp[EC800_RESP_MAX];
    ec800_result_t rc = send_cmd("AT+CSQ", 2000, resp, sizeof(resp));
    if (rc != EC800_OK) return rc;

    int a = 0, b = 0;
    if (sscanf(resp, "+CSQ: %d,%d", &a, &b) == 2) {
        if (rssi) *rssi = a;
        if (ber) *ber = b;
        return EC800_OK;
    }
    return EC800_ERR_MODEM;
}
//Hàm check trạng thái đăng ký mạng
ec800_result_t ec800_get_network_state(ec800_net_state_t *state)
{
    char resp[EC800_RESP_MAX];
    ec800_result_t rc = send_cmd("AT+CREG?", 2000, resp, sizeof(resp));
    if (rc != EC800_OK) return rc;

    int n = 0, stat = 0;
    if (sscanf(resp, "+CREG: %d,%d", &n, &stat) == 2) {
        if (state) {
            switch (stat) {
                case 1: *state = EC800_NET_REGISTERED_HOME; break;
                case 5: *state = EC800_NET_REGISTERED_ROAMING; break;
                default: *state = EC800_NET_NOT_REGISTERED; break;
            }
        }
        return EC800_OK;
    }
    return EC800_ERR_MODEM;
}

// Hàm chờ đăng ký mạng
ec800_result_t ec800_wait_for_network(int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        ec800_net_state_t st = EC800_NET_UNKNOWN;
        if (ec800_get_network_state(&st) == EC800_OK &&
            (st == EC800_NET_REGISTERED_HOME || st == EC800_NET_REGISTERED_ROAMING)) {
            return EC800_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited += 1000;
    }
    return EC800_ERR_TIMEOUT;
}

// Hàm gửi tin nhắn 
ec800_result_t ec800_send_sms(const char *number, const char *text)
{
    if (!number || !text) return EC800_ERR_BAD_STATE;

    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(g.cfg.cmd_timeout_ms)) != pdTRUE) {
        return EC800_ERR_BUSY;
    }

    g.resp_len = 0;
    g.resp_buf[0] = '\0';
    g.waiting_resp = true;
    g.got_final = false;
    g.final_result = EC800_ERR_TIMEOUT;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    ESP_LOGI(TAG, ">> %s", cmd);
    uart_write_bytes(g.cfg.uart_num, cmd, strlen(cmd));
    uart_write_bytes(g.cfg.uart_num, "\r\n", 2);

    vTaskDelay(pdMS_TO_TICKS(500));
    uart_write_bytes(g.cfg.uart_num, text, strlen(text));
    const char ctrlz = 0x1A;
    uart_write_bytes(g.cfg.uart_num, &ctrlz, 1);

    if (xSemaphoreTake(g.resp_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        g.waiting_resp = false;
        xSemaphoreGive(g.lock);
        return EC800_ERR_TIMEOUT;
    }

    g.waiting_resp = false;
    ec800_result_t rc = g.final_result;
    xSemaphoreGive(g.lock);
    return rc;
}

// Hàm gọi
ec800_result_t ec800_dial(const char *number)
{
    if (!number) return EC800_ERR_BAD_STATE;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "ATD%s;", number);

    ec800_result_t rc = send_cmd(cmd, 10000, NULL, 0);
    if (rc == EC800_OK) {
        g.call_state = EC800_CALL_DIALING;
    }
    return rc;
}

// Hàm trả lời cuộc gọi đến
ec800_result_t ec800_answer(void)
{
    ec800_result_t rc = send_cmd("ATA", 10000, NULL, 0);
    if (rc == EC800_OK) {
        g.call_state = EC800_CALL_ACTIVE;
    }
    return rc;
}

// Hàm kết thúc cuộc gội
ec800_result_t ec800_hangup(void)
{
    ec800_result_t rc = send_cmd("ATH", 5000, NULL, 0);
    g.call_state = EC800_CALL_IDLE;
    return rc;
}

// hàm trả về trạng thái cuộc gọi
ec800_call_state_t ec800_get_call_state(void)
{
    return g.call_state;
}