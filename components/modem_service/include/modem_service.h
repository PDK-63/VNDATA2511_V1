#pragma once

#include "esp_err.h"
#include "driver/uart.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MODEM_STATE_OFF = 0,
    MODEM_STATE_INIT,
    MODEM_STATE_SYNC,
    MODEM_STATE_DATA,
    MODEM_STATE_WAIT_IP,
    MODEM_STATE_RUNNING,
    MODEM_STATE_RECOVERING,
} modem_state_t;

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int baudrate;
    const char *apn;
} modem_service_config_t;

// Doc chat luong song
typedef struct {
    int rssi;
    int ber;
    bool valid;
} modem_signal_t;

typedef enum {
    MODEM_RECOVERY_SIM_UNKNOWN = 0,
    MODEM_RECOVERY_SIM_ABSENT,
    MODEM_RECOVERY_SIM_PRESENT,
} modem_recovery_sim_state_t;

typedef struct {
    bool at_seen_this_boot;
    TickType_t last_at_ok_tick;

    bool sim_seen_this_boot;
    modem_recovery_sim_state_t sim_state;
    TickType_t last_sim_ready_tick;

    bool ip_ready;
    bool cs_session_active;
} modem_recovery_status_t;

void modem_service_get_recovery_status(modem_recovery_status_t *out);

bool modem_service_is_sim_ready(void);
void modem_service_set_sim_ready(bool ready);

esp_err_t modem_service_get_signal(modem_signal_t *sig);

esp_err_t modem_service_init(const modem_service_config_t *cfg);
esp_err_t modem_service_start(void);
esp_err_t modem_service_restart_ppp(void);
esp_err_t modem_service_power_cycle_and_restart(void);
esp_err_t modem_service_stop_ppp(void);
bool modem_service_is_ip_ready(void);
modem_state_t modem_service_get_state(void);

esp_err_t modem_service_send_sms(const char *number, const char *text);
esp_err_t modem_service_make_call(const char *number, uint32_t duration_ms);
esp_err_t modem_service_poll_unread_sms(char *number, size_t number_len, char *text, size_t text_len, bool *found);
bool modem_service_should_auto_restart_ppp(void);
bool modem_service_is_cs_session_active(void);
esp_err_t modem_service_delete_all_sms(void);

bool modem_service_is_busy_too_long(uint32_t timeout_ms);
bool modem_service_take_ppp_restart_pending_after_cs(void);