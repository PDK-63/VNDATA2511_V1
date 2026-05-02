#include "modem_status.h"
#include <string.h>

static modem_status_t s_modem_status;

void modem_status_init(void)
{
    memset(&s_modem_status, 0, sizeof(s_modem_status));
}

void modem_status_set_at_ok(bool v)
{
    s_modem_status.at_ok = v;
    if (!v) {
        s_modem_status.sim_ok = false;
        s_modem_status.reg_ok = false;
        s_modem_status.data_ok = false;
    }
}

void modem_status_set_sim_ok(bool v)
{
    s_modem_status.sim_ok = v;
    if (!v) {
        s_modem_status.reg_ok = false;
        s_modem_status.data_ok = false;
    }
}

void modem_status_set_reg_ok(bool v)
{
    s_modem_status.reg_ok = v;
    if (!v) {
        s_modem_status.data_ok = false;
    }
}

void modem_status_set_data_ok(bool v)
{
    s_modem_status.data_ok = v;
}

modem_status_t modem_status_get(void)
{
    return s_modem_status;
}

