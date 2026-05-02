#pragma once

#include <stdbool.h>
#include "tm1638.h"

void status_led_init(tm1638_t *dev);
void status_led_set_sim_ready(bool ready);
void status_led_set_off(void);