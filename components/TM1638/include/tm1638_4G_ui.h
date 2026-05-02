#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "tm1638.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIM_LED_STATE_OFF = 0,       // Khong co SIM / loi SIM / mat ket noi
    SIM_LED_STATE_SEARCHING,     // Dang do SIM / dang dang ky mang / dang cho IP
    SIM_LED_STATE_READY,         // SIM OK / 4G OK
} sim_led_state_t;

/**
 * @brief Gan TM1638 device da init xong cho SIM LED.
 */
void sim_led_bind(tm1638_t *dev);

/**
 * @brief Dat index LED tren TM1638.
 *
 * Neu LED cua ban dung index 6 thi co the khong can goi ham nay,
 * vi mac dinh component dang de SIM_LED_DEFAULT_INDEX = 6.
 */
void sim_led_set_index(uint8_t index);

/**
 * @brief Dat trang thai LED SIM.
 */
void sim_led_set_state(sim_led_state_t state);

/**
 * @brief Lay trang thai hien tai.
 */
sim_led_state_t sim_led_get_state(void);

/**
 * @brief Goi dinh ky moi 100ms trong task UI / app_logic task.
 */
void sim_led_tick_100ms(void);

#ifdef __cplusplus
}
#endif