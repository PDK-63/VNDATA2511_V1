#include "net_stub.h"

#include "esp_log.h"

#include "event_bus.h"
#include "power_monitor.h"

typedef struct {
    event_bus_t *bus;
} net_stub_ctx_t;

static const char *TAG = "pm_net";

static void log_power_event_vi(const power_event_t *evt)
{
    if (evt == NULL) {
        return;
    }

    switch (evt->type) {
        case POWER_EVENT_MAIN_LOST:
            ESP_LOGW(TAG, "MAT DIEN");
            break;

        case POWER_EVENT_MAIN_RESTORED:
            ESP_LOGI(TAG, "CO DIEN");
            break;

        default:
            break;
    }
}

void net_stub_task(void *arg)
{
    net_stub_ctx_t *ctx = (net_stub_ctx_t *)arg;
    power_event_t evt;

    while (1) {
        if (event_bus_receive(ctx->bus, &evt, portMAX_DELAY) == ESP_OK) {
            log_power_event_vi(&evt);

        }
    }
}
