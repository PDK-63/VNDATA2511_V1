#pragma once
#include <stdbool.h>

typedef struct {
    bool at_ok;
    bool sim_ok;
    bool reg_ok;
    bool data_ok;
} modem_status_t;

void modem_status_init(void);
void modem_status_set_at_ok(bool v);
void modem_status_set_sim_ok(bool v);
void modem_status_set_reg_ok(bool v);
void modem_status_set_data_ok(bool v);
modem_status_t modem_status_get(void);