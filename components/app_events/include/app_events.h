// #pragma once

// #include "esp_event.h"
// #include <stdbool.h>

// #ifdef __cplusplus
// extern "C" {
// #endif

// ESP_EVENT_DECLARE_BASE(APP_EVENTS);

// typedef enum {
//     APP_EVENT_NET_UP = 1,
//     APP_EVENT_NET_DOWN,
//     APP_EVENT_MQTT_CONNECTED,
//     APP_EVENT_MQTT_DISCONNECTED,
//     APP_EVENT_CLOUD_COMMAND,
//     APP_EVENT_WIFI_CONFIG_SAVED,
//     APP_EVENT_PROVISION_START,
// } app_event_id_t;

// typedef enum {
//     APP_NET_NONE = 0,
//     APP_NET_AP_ONLY,
//     APP_NET_WIFI,
//     APP_NET_PPP,
// } app_net_type_t;

// typedef struct {
//     app_net_type_t type;
//     bool has_ip;
// } app_net_status_t;

// typedef struct {
//     char request_id[63];
//     char cmd[32];
//     char params[1536];
// } app_cloud_cmd_t;

// #ifdef __cplusplus
// }
// #endif

#pragma once

#include "esp_event.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_EVENTS);

typedef enum {
    APP_EVENT_NET_UP = 1,
    APP_EVENT_NET_DOWN,
    APP_EVENT_MQTT_CONNECTED,
    APP_EVENT_MQTT_DISCONNECTED,
    APP_EVENT_CLOUD_COMMAND,
    APP_EVENT_WIFI_CONFIG_SAVED,
    APP_EVENT_PROVISION_START,
} app_event_id_t;

typedef enum {
    APP_NET_NONE = 0,
    APP_NET_AP_ONLY,
    APP_NET_WIFI,
    APP_NET_ETH,
    APP_NET_PPP,
} app_net_type_t;

typedef struct {
    app_net_type_t type;
    bool has_ip;
} app_net_status_t;

typedef struct {
    char request_id[63];
    char cmd[32];
    char params[1536];
} app_cloud_cmd_t;

#ifdef __cplusplus
}
#endif