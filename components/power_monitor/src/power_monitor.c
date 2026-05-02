#include "power_monitor.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "power_monitor_config.h"
#include "adc_service.h"
#include "event_bus.h"
#include "net_stub.h"

typedef struct {
    power_state_t state;
    uint32_t lost_counter;
    uint32_t restore_counter;
    bool bk_low_reported;
    power_sample_t latest_sample;
    bool started;
    power_monitor_event_cb_t cb;
    void *cb_user_ctx;
    adc_service_t adc;
    event_bus_t bus;
} power_monitor_ctx_t;

static const char *TAG = "power_monitor";
static power_monitor_ctx_t g_pm;

typedef struct {
    event_bus_t *bus;
} net_stub_ctx_t;

static net_stub_ctx_t g_net_stub_ctx;

static power_state_t derive_initial_state(const power_sample_t *s)
{
    if (s->main_v >= PM_MAIN_RESTORE_THRESHOLD_V) {
        return POWER_STATE_MAIN_OK;
    }
    if (s->main_v <= PM_MAIN_LOST_THRESHOLD_V) {
        return POWER_STATE_MAIN_LOST;
    }
    return POWER_STATE_UNKNOWN;
}

const char *power_monitor_state_to_str(power_state_t state)
{
    switch (state) {
        case POWER_STATE_MAIN_OK:   return "MAIN_OK";
        case POWER_STATE_MAIN_LOST: return "MAIN_LOST";
        default:                    return "UNKNOWN";
    }
}

const char *power_monitor_event_to_str(power_event_type_t type)
{
    switch (type) {
        case POWER_EVENT_MAIN_LOST:     return "MAIN_LOST";
        case POWER_EVENT_MAIN_RESTORED: return "MAIN_RESTORED";
        case POWER_EVENT_BK_LOW:        return "BK_LOW";
        default:                        return "NONE";
    }
}

static void emit_callback_if_registered(const power_event_t *evt)
{
    if (g_pm.cb != NULL && evt != NULL) {
        g_pm.cb(evt, g_pm.cb_user_ctx);
    }
}

static esp_err_t publish_and_notify(power_event_type_t type,
                                    float main_v,
                                    float bk_v,
                                    int64_t timestamp_us)
{
    ESP_RETURN_ON_ERROR(event_bus_publish(&g_pm.bus, type, main_v, bk_v, timestamp_us),
                        TAG, "publish event failed");

    power_event_t evt = {
        .type = type,
        .main_v = main_v,
        .bk_v = bk_v,
        .timestamp_us = timestamp_us,
        .seq = g_pm.bus.next_seq - 1,
    };
    emit_callback_if_registered(&evt);
    return ESP_OK;
}

static esp_err_t process_sample(const power_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    g_pm.latest_sample = *sample;

    if (g_pm.state == POWER_STATE_UNKNOWN) {
        g_pm.state = derive_initial_state(sample);
        return ESP_OK;
    }

    switch (g_pm.state) {
        case POWER_STATE_MAIN_OK:
            if (sample->main_v < PM_MAIN_LOST_THRESHOLD_V) {
                g_pm.lost_counter++;
                g_pm.restore_counter = 0;
                if (g_pm.lost_counter >= PM_MAIN_DEBOUNCE_COUNT) {
                    g_pm.state = POWER_STATE_MAIN_LOST;
                    g_pm.lost_counter = 0;
                    g_pm.bk_low_reported = false;
                    ESP_RETURN_ON_ERROR(publish_and_notify(POWER_EVENT_MAIN_LOST,
                                                           sample->main_v,
                                                           sample->bk_v,
                                                           sample->timestamp_us),
                                        TAG, "notify main lost failed");
                }
            } else {
                g_pm.lost_counter = 0;
            }
            break;

        case POWER_STATE_MAIN_LOST:
            if (sample->main_v > PM_MAIN_RESTORE_THRESHOLD_V) {
                g_pm.restore_counter++;
                g_pm.lost_counter = 0;
                if (g_pm.restore_counter >= PM_MAIN_DEBOUNCE_COUNT) {
                    g_pm.state = POWER_STATE_MAIN_OK;
                    g_pm.restore_counter = 0;
                    g_pm.bk_low_reported = false;
                    ESP_RETURN_ON_ERROR(publish_and_notify(POWER_EVENT_MAIN_RESTORED,
                                                           sample->main_v,
                                                           sample->bk_v,
                                                           sample->timestamp_us),
                                        TAG, "notify main restored failed");
                }
            } else {
                g_pm.restore_counter = 0;
            }

            if ((sample->bk_v > PM_BK_PRESENT_THRESHOLD_V) &&
                (sample->bk_v < PM_BK_LOW_THRESHOLD_V) &&
                (!g_pm.bk_low_reported)) {
                g_pm.bk_low_reported = true;
                ESP_RETURN_ON_ERROR(publish_and_notify(POWER_EVENT_BK_LOW,
                                                       sample->main_v,
                                                       sample->bk_v,
                                                       sample->timestamp_us),
                                    TAG, "notify bk low failed");
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        power_sample_t sample = {0};
        esp_err_t err = adc_service_read_sample(&g_pm.adc, &sample);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc read failed: %s", esp_err_to_name(err));
        } else {
            err = process_sample(&sample);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "process sample failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG,
                         "state=%s Vmain=%.2fV Vbk=%.2fV",
                         power_monitor_state_to_str(g_pm.state),
                         sample.main_v,
                         sample.bk_v);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PM_MONITOR_PERIOD_MS));
    }
}

esp_err_t power_monitor_init(void)
{
    memset(&g_pm, 0, sizeof(g_pm));
    g_pm.state = POWER_STATE_UNKNOWN;

    ESP_RETURN_ON_ERROR(adc_service_init(&g_pm.adc), TAG, "adc init failed");
    ESP_RETURN_ON_ERROR(event_bus_init(&g_pm.bus, PM_EVENT_QUEUE_LEN), TAG, "event bus init failed");
    return ESP_OK;
}

esp_err_t power_monitor_start(void)
{
    if (g_pm.started) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok1 = xTaskCreatePinnedToCore(monitor_task,
                                             "pm_monitor",
                                             PM_MONITOR_TASK_STACK,
                                             NULL,
                                             PM_MONITOR_TASK_PRIORITY,
                                             NULL,
                                             tskNO_AFFINITY);
    if (ok1 != pdPASS) {
        return ESP_FAIL;
    }

    g_net_stub_ctx.bus = &g_pm.bus;
    BaseType_t ok2 = xTaskCreatePinnedToCore(net_stub_task,
                                             "pm_net",
                                             PM_NETWORK_TASK_STACK,
                                             &g_net_stub_ctx,
                                             PM_NETWORK_TASK_PRIORITY,
                                             NULL,
                                             tskNO_AFFINITY);
    if (ok2 != pdPASS) {
        return ESP_FAIL;
    }

    g_pm.started = true;
    return ESP_OK;
}

esp_err_t power_monitor_register_callback(power_monitor_event_cb_t cb, void *user_ctx)
{
    g_pm.cb = cb;
    g_pm.cb_user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t power_monitor_get_latest(power_sample_t *sample, power_state_t *state)
{
    if (sample == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *sample = g_pm.latest_sample;
    *state = g_pm.state;
    return ESP_OK;
}

adc_oneshot_unit_handle_t power_monitor_get_adc_handle(void)
{
    return g_pm.adc.adc_handle;
}
