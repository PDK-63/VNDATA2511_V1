#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_system.h"
#include "board.h"

typedef struct {
    uint32_t boot_count;
    uint32_t unclean_reset_count;
    esp_reset_reason_t reset_reason;
    uint32_t sync_fail;
    uint32_t ppp_restart;
    uint32_t mqtt_restart;
    uint32_t modem_power_cycle;
} diag_snapshot_t;

esp_err_t diag_service_init(void);
void diag_inc_sync_fail(void);
void diag_inc_ppp_restart(void);
void diag_inc_mqtt_restart(void);
void diag_inc_modem_power_cycle(void);
void diag_mark_clean_shutdown(void);
void diag_get_snapshot(diag_snapshot_t *out);
int diag_build_attributes_json(char *buf, size_t len);
void diag_log_health(const char *reason, bool ip_ready, bool mqtt_ready, int outbox_size);
