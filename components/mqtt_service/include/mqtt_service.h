#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *birth_payload;
    const char *lwt_payload;
} mqtt_service_config_t;

esp_err_t mqtt_service_init(const mqtt_service_config_t *cfg);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);
int mqtt_service_publish_telemetry(const char *json);
int mqtt_service_publish_attributes(const char *json);
int mqtt_service_publish_status(const char *json);
int mqtt_service_publish_reply(const char *request_id, const char *json);
int mqtt_service_get_outbox_size(void);
bool mqtt_service_is_connected(void);
bool mqtt_service_is_started(void);
const char *mqtt_service_get_device_key(void);
