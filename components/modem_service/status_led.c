#include "status_led.h"

#define TM1638_SIM_LED_INDEX 2

static tm1638_t *s_tm1638_dev = NULL;

void status_led_init(tm1638_t *dev)
{
    s_tm1638_dev = dev;
    if (s_tm1638_dev) {
        (void)tm1638_set_led(s_tm1638_dev, TM1638_SIM_LED_INDEX, false);
    }
}

void status_led_set_sim_ready(bool ready)
{
    if (!s_tm1638_dev) {
        return;
    }
    (void)tm1638_set_led(s_tm1638_dev, TM1638_SIM_LED_INDEX, ready);
}

void status_led_set_off(void)
{
    status_led_set_sim_ready(false);
}