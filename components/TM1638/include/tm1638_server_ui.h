#pragma once

#include <stdbool.h>
#include "tm1638.h"

typedef enum {
    SERVER_LED_OFF = 0,
    SERVER_LED_ERROR_BLINK,
    SERVER_LED_CONNECTED,
} server_led_state_t;

void tm1638_server_bind(tm1638_t *dev);
void tm1638_server_set_state(server_led_state_t state);
server_led_state_t tm1638_server_get_state(void);
void tm1638_server_led_tick_100ms(void);