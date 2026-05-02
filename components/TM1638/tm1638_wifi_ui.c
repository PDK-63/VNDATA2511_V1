#include "tm1638_wifi_ui.h"
#include "esp_log.h"

static const char *TAG = "tm1638_wifi_ui";

/* LED so 06 tu trai qua phai -> index 5 neu driver 0-based */
#define TM1638_WIFI_LED_INDEX 5

static tm1638_t *s_tm1638_dev = NULL;
static wifi_led_state_t s_state = WIFI_LED_OFF;
static unsigned s_tick = 0;

static void wifi_led_apply(bool on)
{
    if (s_tm1638_dev) {
        tm1638_set_led(s_tm1638_dev, TM1638_WIFI_LED_INDEX, on);
    }
}

void tm1638_wifi_bind(tm1638_t *dev)
{
    s_tm1638_dev = dev;
    ESP_LOGI(TAG, "tm1638 wifi bind dev=%p", (void *)dev);

    wifi_led_apply(false);
}

void tm1638_wifi_set_state(wifi_led_state_t state)
{
    //ESP_LOGI(TAG, "wifi led state change: %d -> %d", (int)s_state, (int)state);

    if (s_state == state) {
        return;
    }

    s_state = state;
    s_tick = 0;

    switch (s_state) {
    case WIFI_LED_OFF:
        wifi_led_apply(false);
        break;

    case WIFI_LED_READY:
        wifi_led_apply(true);
        break;

    case WIFI_LED_SCANNING:
    case WIFI_LED_CONFIG_MODE:
        wifi_led_apply(true);
        break;

    default:
        wifi_led_apply(false);
        break;
    }
}

wifi_led_state_t tm1638_wifi_get_state(void)
{
    return s_state;
}

void tm1638_wifi_led_tick_100ms(void)
{
    if (!s_tm1638_dev) {
        return;
    }

    s_tick++;

    bool on = false;

    switch (s_state) {
    case WIFI_LED_OFF:
        on = false;
        break;

    case WIFI_LED_SCANNING:
        on = ((s_tick / 5U) % 2U) == 0U;
        break;

    case WIFI_LED_CONFIG_MODE:
        on = ((s_tick / 2U) % 2U) == 0U;
        break;

    case WIFI_LED_READY:
        on = true;
        break;

    default:
        on = false;
        break;
    }

    // if ((s_tick % 10U) == 0U) {
    //     ESP_LOGI(TAG, "tick state=%d tick=%u on=%d", (int)s_state, s_tick, (int)on);
    // }

    wifi_led_apply(on);
}