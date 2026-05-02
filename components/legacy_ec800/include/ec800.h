#pragma once

#include "ec800_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ec800_urc_cb_t)(const char *line, void *ctx);

esp_err_t ec800_init(const ec800_config_t *cfg, ec800_urc_cb_t urc_cb, void *urc_ctx);
void ec800_deinit(void);

ec800_result_t ec800_start(void);
ec800_result_t ec800_get_signal(int *rssi, int *ber);
ec800_result_t ec800_get_network_state(ec800_net_state_t *state);
ec800_result_t ec800_wait_for_network(int timeout_ms);

ec800_result_t ec800_send_sms(const char *number, const char *text);
ec800_result_t ec800_dial(const char *number);
ec800_result_t ec800_answer(void);
ec800_result_t ec800_hangup(void);
ec800_call_state_t ec800_get_call_state(void);

#ifdef __cplusplus
}
#endif