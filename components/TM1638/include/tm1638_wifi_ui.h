#pragma once

#include <stdbool.h>
#include "tm1638.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_LED_OFF = 0,        /* tat */
    WIFI_LED_SCANNING,       /* dang do -> nhap nhay cham */
    WIFI_LED_CONFIG_MODE,    /* dang cau hinh -> nhap nhay nhanh */
    WIFI_LED_READY,          /* ket noi ok -> sang lien tuc */
} wifi_led_state_t;

/* Gan instance TM1638 da init xong */
void tm1638_wifi_bind(tm1638_t *dev);

/* Dat / doc state LED Wi-Fi */
void tm1638_wifi_set_state(wifi_led_state_t state);
wifi_led_state_t tm1638_wifi_get_state(void);

/* Goi dinh ky moi 100 ms trong task UI */
void tm1638_wifi_led_tick_100ms(void);

#ifdef __cplusplus
}
#endif
