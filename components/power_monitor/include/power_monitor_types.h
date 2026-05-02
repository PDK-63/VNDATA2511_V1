#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_STATE_UNKNOWN = 0,
    POWER_STATE_MAIN_OK = 1,
    POWER_STATE_MAIN_LOST = 2,
} power_state_t;

typedef enum {
    POWER_EVENT_NONE = 0,
    POWER_EVENT_MAIN_LOST,
    POWER_EVENT_MAIN_RESTORED,
    POWER_EVENT_BK_LOW,
} power_event_type_t;

typedef struct {
    float main_v;
    float bk_v;
    int64_t timestamp_us;
} power_sample_t;

typedef struct {
    power_event_type_t type;
    float main_v;
    float bk_v;
    int64_t timestamp_us;
    uint32_t seq;
} power_event_t;

#ifdef __cplusplus
}
#endif
