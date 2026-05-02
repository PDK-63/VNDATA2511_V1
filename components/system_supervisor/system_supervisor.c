#include "system_supervisor.h"
#include "app_config.h"
#include "app_events.h"
#include "diag_service.h"
#include "mqtt_service.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "supervisor";

static TaskHandle_t s_task;
static TickType_t s_provision_btn_press_tick;
static bool s_provision_btn_latched;

static volatile bool s_net_ready;
static volatile bool s_mqtt_ready;
static volatile app_net_type_t s_net_type;

static void provision_button_init(void)
{
    if (APP_PROVISION_BUTTON_GPIO == GPIO_NUM_NC) {
        return;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_PROVISION_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void handle_provision_button(void)
{
    if (APP_PROVISION_BUTTON_GPIO == GPIO_NUM_NC) {
        return;
    }

    int level = gpio_get_level(APP_PROVISION_BUTTON_GPIO);
    bool pressed = (level == APP_PROVISION_BUTTON_ACTIVE_LEVEL);
    TickType_t now = xTaskGetTickCount();

    if (!pressed) {
        s_provision_btn_press_tick = 0;
        s_provision_btn_latched = false;
        return;
    }

    if (s_provision_btn_press_tick == 0) {
        s_provision_btn_press_tick = now;
        return;
    }

    if (!s_provision_btn_latched &&
        (now - s_provision_btn_press_tick) >= pdMS_TO_TICKS(APP_PROVISION_BUTTON_HOLD_MS)) {
        s_provision_btn_latched = true;
        ESP_LOGW(TAG, "provision button held -> request Wi-Fi reconfiguration");
        esp_event_post(APP_EVENTS, APP_EVENT_PROVISION_START, NULL, 0, portMAX_DELAY);
    }
}

static void on_app_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base != APP_EVENTS) {
        return;
    }

    switch (id) {
    case APP_EVENT_NET_UP: {
        app_net_status_t *st = (app_net_status_t *)data;
        s_net_ready = true;
        if (st) {
            s_net_type = st->type;
        }

        break;
    }

    case APP_EVENT_NET_DOWN: {
        app_net_status_t *st = (app_net_status_t *)data;
        s_net_ready = false;
        s_mqtt_ready = false;

        if (st) {
            s_net_type = st->type;
        } else {
            s_net_type = APP_NET_NONE;
        }
        break;
    }

    case APP_EVENT_MQTT_CONNECTED:
        s_mqtt_ready = true;
        break;

    case APP_EVENT_MQTT_DISCONNECTED:
        s_mqtt_ready = false;
        break;

    default:
        break;
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;

    TickType_t last_health = xTaskGetTickCount();
    s_net_type = APP_NET_NONE;
    s_net_ready = false;
    s_mqtt_ready = false;
    s_provision_btn_press_tick = 0;
    s_provision_btn_latched = false;
    provision_button_init();

    while (1) {
        TickType_t now = xTaskGetTickCount();
        handle_provision_button();

        /*
         * net_manager là owner duy nhất điều phối PPP/Wi-Fi/MQTT.
         * Supervisor chỉ ghi health log và xử lý nút provisioning,
         * tránh restart PPP song song gây state machine chồng chéo.
         */
        (void)now;

        if ((now - last_health) >= pdMS_TO_TICKS(APP_HEALTH_LOG_MS)) {
            diag_log_health(
                "supervisor",
                s_net_ready,
                mqtt_service_is_connected(),
                mqtt_service_get_outbox_size()
            );
            last_health = now;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_PROVISION_BUTTON_POLL_MS));
    }
}

esp_err_t system_supervisor_init(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID, on_app_event, NULL));
    return ESP_OK;
}

esp_err_t system_supervisor_start(void)
{
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(supervisor_task, "supervisor_task", 4096, NULL, 9, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}