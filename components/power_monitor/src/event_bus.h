#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "power_monitor_types.h"

typedef struct {
    QueueHandle_t queue;
    uint32_t next_seq;
} event_bus_t;

esp_err_t event_bus_init(event_bus_t *bus, uint32_t queue_len);
esp_err_t event_bus_publish(event_bus_t *bus,
                            power_event_type_t type,
                            float main_v,
                            float bk_v,
                            int64_t timestamp_us);
esp_err_t event_bus_receive(event_bus_t *bus,
                            power_event_t *event,
                            TickType_t ticks_to_wait);
