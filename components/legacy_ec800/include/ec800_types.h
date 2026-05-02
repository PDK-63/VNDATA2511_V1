#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    EC800_OK = 0,
    EC800_ERR_TIMEOUT,
    EC800_ERR_IO,
    EC800_ERR_MODEM,
    EC800_ERR_BUSY,
    EC800_ERR_BAD_STATE,
    EC800_ERR_UNSUPPORTED,
} ec800_result_t;

typedef enum {
    EC800_CALL_IDLE = 0,
    EC800_CALL_DIALING,
    EC800_CALL_RINGING,
    EC800_CALL_ACTIVE,
} ec800_call_state_t;

typedef enum {
    EC800_NET_UNKNOWN = 0,
    EC800_NET_NOT_REGISTERED,
    EC800_NET_REGISTERED_HOME,
    EC800_NET_REGISTERED_ROAMING,
} ec800_net_state_t;

typedef struct {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    int rx_buf_size;
    int tx_buf_size;
    int cmd_timeout_ms;
} ec800_config_t;