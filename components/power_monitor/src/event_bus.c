#include "event_bus.h"

#include <string.h>

esp_err_t event_bus_init(event_bus_t *bus, uint32_t queue_len)
{
    if (bus == NULL || queue_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(bus, 0, sizeof(*bus));
    bus->queue = xQueueCreate(queue_len, sizeof(power_event_t));
    if (bus->queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bus->next_seq = 1;
    return ESP_OK;
}

esp_err_t event_bus_publish(event_bus_t *bus,
                            power_event_type_t type,
                            float main_v,
                            float bk_v,
                            int64_t timestamp_us)
{
    if (bus == NULL || bus->queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    power_event_t evt = {
        .type = type,
        .main_v = main_v,
        .bk_v = bk_v,
        .timestamp_us = timestamp_us,
        .seq = bus->next_seq++,
    };

    if (xQueueSend(bus->queue, &evt, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t event_bus_receive(event_bus_t *bus,
                            power_event_t *event,
                            TickType_t ticks_to_wait)
{
    if (bus == NULL || bus->queue == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(bus->queue, event, ticks_to_wait) == pdTRUE) {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}
