#include "tm1638_4G_ui.h"

#include "esp_log.h"

static const char *TAG = "sim_led";

/*
 * LED SIM tren TM1638.
 * Theo thong tin ban noi truoc do: LED o index 6.
 *
 * Luu y:
 * - Driver tm1638_set_led(dev, led, on) dang dung index 0-based.
 * - Neu ban noi "LED so 6 tu trai qua phai" thi index co the la 5.
 * - Neu ban da test va dung la index 6 thi giu nguyen.
 */
#define SIM_LED_DEFAULT_INDEX   6

static tm1638_t *s_tm1638_dev = NULL;
static uint8_t s_led_index = SIM_LED_DEFAULT_INDEX;
static sim_led_state_t s_state = SIM_LED_STATE_OFF;
static uint32_t s_tick = 0;

static void sim_led_apply(bool on)
{
    if (!s_tm1638_dev) {
        return;
    }

    if (s_led_index >= TM1638_NUM_LEDS) {
        return;
    }

    (void)tm1638_set_led(s_tm1638_dev, s_led_index, on);
}

void sim_led_bind(tm1638_t *dev)
{
    s_tm1638_dev = dev;
    s_tick = 0;

    ESP_LOGI(TAG, "bind TM1638 dev=%p led_index=%u",
             (void *)dev,
             (unsigned)s_led_index);

    sim_led_apply(false);
}

void sim_led_set_index(uint8_t index)
{
    if (index >= TM1638_NUM_LEDS) {
        ESP_LOGW(TAG, "invalid led index=%u, max=%u",
                 (unsigned)index,
                 (unsigned)(TM1638_NUM_LEDS - 1));
        return;
    }

    s_led_index = index;
    s_tick = 0;

    ESP_LOGI(TAG, "set led index=%u", (unsigned)s_led_index);

    sim_led_set_state(s_state);
}

void sim_led_set_state(sim_led_state_t state)
{
    if (s_state == state) {
        return;
    }

    s_state = state;
    s_tick = 0;

    switch (s_state) {
    case SIM_LED_STATE_OFF:
        sim_led_apply(false);
        break;

    case SIM_LED_STATE_SEARCHING:
        sim_led_apply(true);
        break;

    case SIM_LED_STATE_READY:
        sim_led_apply(true);
        break;

    default:
        sim_led_apply(false);
        break;
    }
}

sim_led_state_t sim_led_get_state(void)
{
    return s_state;
}

void sim_led_tick_100ms(void)
{
    if (!s_tm1638_dev) {
        return;
    }

    s_tick++;

    bool on = false;

    switch (s_state) {
    case SIM_LED_STATE_OFF:
        on = false;
        break;

    case SIM_LED_STATE_SEARCHING:
        /*
         * Nhay cham: 500ms sang / 500ms tat
         * Vi ham nay duoc goi moi 100ms nen 5 tick = 500ms.
         */
        on = ((s_tick / 5U) % 2U) == 0U;
        break;

    case SIM_LED_STATE_READY:
        on = true;
        break;

    default:
        on = false;
        break;
    }

    sim_led_apply(on);
}