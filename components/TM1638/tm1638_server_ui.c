#include "tm1638_server_ui.h"
#include "esp_log.h"

static const char *TAG = "tm1638_server_ui";

static tm1638_t *s_tm1638_dev = NULL;
static server_led_state_t s_state = SERVER_LED_OFF;
static unsigned s_tick = 0;
static bool s_blink_on = false;

static void apply_led8(tm1638_led8_color_t color)
{
    if (s_tm1638_dev) {
        tm1638_set_led8(s_tm1638_dev, color);
    }
}

void tm1638_server_bind(tm1638_t *dev)
{
    s_tm1638_dev = dev;
    s_state = SERVER_LED_OFF;
    s_tick = 0;
    s_blink_on = false;

    ESP_LOGI(TAG, "server led bind dev=%p", (void *)dev);
    apply_led8(TM1638_LED8_OFF);
}

void tm1638_server_set_state(server_led_state_t state)
{
    if (s_state == state) {
        return;
    }

    s_state = state;
    s_tick = 0;
    s_blink_on = false;

    ESP_LOGI(TAG, "server led state=%d", (int)state);

    switch (s_state) {
    case SERVER_LED_CONNECTED:
        apply_led8(TM1638_LED8_GREEN);
        break;

    case SERVER_LED_ERROR_BLINK:
        apply_led8(TM1638_LED8_OFF);
        break;

    case SERVER_LED_OFF:
    default:
        apply_led8(TM1638_LED8_OFF);
        break;
    }
}

server_led_state_t tm1638_server_get_state(void)
{
    return s_state;
}

void tm1638_server_led_tick_100ms(void)
{
    if (!s_tm1638_dev) {
        return;
    }

    switch (s_state) {
    case SERVER_LED_CONNECTED:
        apply_led8(TM1638_LED8_GREEN);
        break;

    case SERVER_LED_ERROR_BLINK:
        s_tick++;
        if (s_tick >= 5) {   // 500ms dao trang thai
            s_tick = 0;
            s_blink_on = !s_blink_on;
            apply_led8(s_blink_on ? TM1638_LED8_RED : TM1638_LED8_OFF);
        }
        break;

    case SERVER_LED_OFF:
    default:
        apply_led8(TM1638_LED8_OFF);
        break;
    }
}

