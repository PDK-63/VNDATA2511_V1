#include "mqtt_service.h"
#include "app_config.h"
#include "app_events.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "mqtt_service";
static esp_mqtt_client_handle_t s_mqtt;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_started;
static bool s_connected;
static bool s_connecting;
static char s_broker_uri[160];
static char s_username[128];
static char s_password[128];
static char s_birth_payload[160];
static char s_lwt_payload[160];
static char s_topic_status[128];
static char s_topic_state[128];
static char s_topic_telemetry[128];
static char s_topic_cmd[128];
static char s_topic_cmd_reply[128];
static char s_topic_settings[128];
static char s_device_key[32];

const char *mqtt_service_get_device_key(void)
{
    return s_device_key;
}

static bool copy_buf_to_cstr(const char *src, int src_len, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0 || src_len < 0) return false;
    size_t n = (size_t)src_len;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return true;
}

static bool parse_command_payload(const char *payload, app_cloud_cmd_t *out)
{
    if (!payload || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "parse_command_payload: invalid json");
        return false;
    }

    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (!cJSON_IsString(cmd) || !cmd->valuestring || cmd->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "parse_command_payload: missing cmd");
        cJSON_Delete(root);
        return false;
    }

    if (cJSON_IsString(request_id) && request_id->valuestring && request_id->valuestring[0] != '\0') {
        strlcpy(out->request_id, request_id->valuestring, sizeof(out->request_id));
    } else {
        strlcpy(out->request_id, "web", sizeof(out->request_id));
    }

    strlcpy(out->cmd, cmd->valuestring, sizeof(out->cmd));

    if (params != NULL) {
        char *json = cJSON_PrintUnformatted(params);
        if (!json) {
            cJSON_Delete(root);
            return false;
        }

        strlcpy(out->params, json, sizeof(out->params));
        cJSON_free(json);
        cJSON_Delete(root);
        return true;
    }

    cJSON *flat_params = cJSON_CreateObject();
    if (!flat_params) {
        cJSON_Delete(root);
        return false;
    }

    for (cJSON *item = root->child; item != NULL; item = item->next) {
        if (!item->string) {
            continue;
        }

        if (strcmp(item->string, "request_id") == 0 ||
            strcmp(item->string, "cmd") == 0) {
            continue;
        }

        cJSON *dup = cJSON_Duplicate(item, 1);
        if (!dup) {
            cJSON_Delete(flat_params);
            cJSON_Delete(root);
            return false;
        }

        cJSON_AddItemToObject(flat_params, item->string, dup);
    }

    char *json = cJSON_PrintUnformatted(flat_params);
    cJSON_Delete(flat_params);
    cJSON_Delete(root);

    if (!json) {
        return false;
    }

    strlcpy(out->params, json, sizeof(out->params));
    cJSON_free(json);
    return true;
}

static void handle_cmd_topic(const char *payload)
{
    app_cloud_cmd_t cmd = {0};

    if (!parse_command_payload(payload, &cmd)) {
        ESP_LOGW(TAG, "invalid command payload: %s", payload ? payload : "(null)");
        return;
    }

    ESP_LOGI(TAG, "CMD id=%s cmd=%s params=%s",
             cmd.request_id,
             cmd.cmd,
             cmd.params);

    esp_event_post(APP_EVENTS, APP_EVENT_CLOUD_COMMAND, &cmd, sizeof(cmd), portMAX_DELAY);
}

static bool mqtt_extract_host(const char *uri, char *host, size_t host_size)
{
    if (!uri || !host || host_size == 0) {
        return false;
    }

    const char *p = strstr(uri, "://");
    p = p ? (p + 3) : uri;

    if (*p == '\0') {
        return false;
    }

    const char *at = strchr(p, '@');
    if (at) {
        p = at + 1;
    }

    const char *end = p;
    while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#') {
        end++;
    }

    if (end == p) {
        return false;
    }

    size_t n = (size_t)(end - p);
    if (n >= host_size) {
        n = host_size - 1;
    }

    memcpy(host, p, n);
    host[n] = '\0';
    return true;
}

static void mqtt_debug_dns_from_uri(const char *uri)
{
    char host[96] = {0};

    if (!mqtt_extract_host(uri, host, sizeof(host))) {
        ESP_LOGW(TAG, "DNS precheck skipped, failed to parse host from uri=[%s]",
                 uri ? uri : "(null)");
        return;
    }

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    int rc = getaddrinfo(host, NULL, &hints, &res);
    ESP_LOGI(TAG, "DNS precheck host=[%s] rc=%d uri=[%s]", host, rc, uri ? uri : "(null)");

    if (res) {
        freeaddrinfo(res);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    if (!e || !e->client) return;

    bool is_current = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    is_current = (e->client == s_mqtt);
    if (s_lock) xSemaphoreGive(s_lock);

    if (!is_current) {
        ESP_LOGW(TAG, "ignore mqtt event from stale client");
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            s_connected = true;
            s_connecting = false;
            s_started = true;
            if (s_lock) xSemaphoreGive(s_lock);

            ESP_LOGI(TAG, "MQTT connected");

            esp_mqtt_client_subscribe(e->client, s_topic_cmd, 1);
            ESP_LOGI(TAG, "subscribed topic=%s", s_topic_cmd);

            esp_mqtt_client_subscribe(e->client, s_topic_settings, 1);
            ESP_LOGI(TAG, "subscribed topic=%s", s_topic_settings);

            if (s_birth_payload[0]) {
                mqtt_service_publish_status(s_birth_payload);
            }

            esp_event_post(APP_EVENTS, APP_EVENT_MQTT_CONNECTED, NULL, 0, portMAX_DELAY);
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            if (e->client == s_mqtt) {
                s_connected = false;
                s_connecting = false;
                s_started = false;
            }
            if (s_lock) xSemaphoreGive(s_lock);

            ESP_LOGW(TAG, "MQTT disconnected");
            esp_event_post(APP_EVENTS, APP_EVENT_MQTT_DISCONNECTED, NULL, 0, portMAX_DELAY);
            break;

        case MQTT_EVENT_DATA: {
            char topic[160];
            char payload[1024];

            if (!copy_buf_to_cstr(e->topic, e->topic_len, topic, sizeof(topic))) return;
            if (!copy_buf_to_cstr(e->data, e->data_len, payload, sizeof(payload))) return;

            ESP_LOGI(TAG, "RX topic=%s payload=%s", topic, payload);

            if (strcmp(topic, s_topic_cmd) == 0 || strcmp(topic, s_topic_settings) == 0) {
                handle_cmd_topic(payload);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            if (e->client == s_mqtt) {
                s_connecting = false;
                s_connected = false;
                s_started = false;
            }
            if (s_lock) xSemaphoreGive(s_lock);

            ESP_LOGE(TAG, "MQTT error type=%d",
                    e->error_handle ? e->error_handle->error_type : -1);
            esp_event_post(APP_EVENTS, APP_EVENT_MQTT_DISCONNECTED, NULL, 0, portMAX_DELAY);
            break;

        default:
            break;
    }
}

int mqtt_service_publish_telemetry(const char *json)
{
    esp_mqtt_client_handle_t client = NULL;
    bool connected = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    client = s_mqtt;
    connected = s_connected;
    if (s_lock) xSemaphoreGive(s_lock);

    if (!client || !connected || !json) return -1;
    return esp_mqtt_client_publish(client, s_topic_telemetry, json, 0, 1, 0);
}

int mqtt_service_publish_attributes(const char *json)
{
    esp_mqtt_client_handle_t client = NULL;
    bool connected = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    client = s_mqtt;
    connected = s_connected;
    if (s_lock) xSemaphoreGive(s_lock);

    if (!client || !connected || !json) return -1;
    return esp_mqtt_client_publish(client, s_topic_state, json, 0, 1, 1);
}

int mqtt_service_publish_status(const char *json)
{
    esp_mqtt_client_handle_t client = NULL;
    bool connected = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    client = s_mqtt;
    connected = s_connected;
    if (s_lock) xSemaphoreGive(s_lock);

    if (!client || !connected || !json) return -1;
    return esp_mqtt_client_publish(client, s_topic_status, json, 0, 1, 1);
}

int mqtt_service_publish_reply(const char *request_id, const char *json)
{
    esp_mqtt_client_handle_t client = NULL;
    bool connected = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    client = s_mqtt;
    connected = s_connected;
    if (s_lock) xSemaphoreGive(s_lock);

    if (!client || !connected || !json || !s_topic_cmd_reply[0]) {
        ESP_LOGE(TAG, "publish_reply invalid state client=%p connected=%d topic=[%s] json=%p",
                 (void *)client, connected ? 1 : 0, s_topic_cmd_reply, (const void *)json);
        return -1;
    }

    if (!request_id) request_id = "";

    // char payload[512];
    // snprintf(payload, sizeof(payload),
    //          "{\"device_id\":\"%s\",\"request_id\":\"%s\",%s}",
    //          s_device_key, request_id, json);

    char payload[2048];

    int n = snprintf(payload, sizeof(payload),
                    "{\"device_id\":\"%s\",\"request_id\":\"%s\",%s}",
                    s_device_key, request_id, json);

    if (n < 0 || n >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "publish_reply payload too long, len=%d", n);
        return -1;
    }
    ESP_LOGW(TAG, "publish_reply topic=%s payload=%s", s_topic_cmd_reply, payload);

    int mid = esp_mqtt_client_publish(client, s_topic_cmd_reply, payload, 0, 1, 0);
    ESP_LOGW(TAG, "publish_reply mid=%d", mid);

    return mid;
}

esp_err_t mqtt_service_init(const mqtt_service_config_t *cfg)
{
    if (!cfg || !cfg->broker_uri) return ESP_ERR_INVALID_ARG;

    memset(s_broker_uri, 0, sizeof(s_broker_uri));
    memset(s_username, 0, sizeof(s_username));
    memset(s_password, 0, sizeof(s_password));
    memset(s_birth_payload, 0, sizeof(s_birth_payload));
    memset(s_lwt_payload, 0, sizeof(s_lwt_payload));

    snprintf(s_broker_uri, sizeof(s_broker_uri), "%s", cfg->broker_uri);
    snprintf(s_username, sizeof(s_username), "%s", cfg->username ? cfg->username : "");
    snprintf(s_password, sizeof(s_password), "%s", cfg->password ? cfg->password : "");
    snprintf(s_birth_payload, sizeof(s_birth_payload), "%s",
             cfg->birth_payload ? cfg->birth_payload : APP_MQTT_BIRTH_PAYLOAD);
    snprintf(s_lwt_payload, sizeof(s_lwt_payload), "%s",
             cfg->lwt_payload ? cfg->lwt_payload : APP_MQTT_LWT_PAYLOAD);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    snprintf(s_device_key, sizeof(s_device_key),
            "%s%02X%02X%02X%02X%02X%02X",
            APP_DEVICE_ID, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_topic_status, sizeof(s_topic_status), APP_MQTT_TOPIC_STATUS_FMT, APP_TOPIC_PREFIX, s_device_key);
    snprintf(s_topic_state, sizeof(s_topic_state), APP_MQTT_TOPIC_STATE_FMT, APP_TOPIC_PREFIX, s_device_key);
    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), APP_MQTT_TOPIC_TELEMETRY_FMT, APP_TOPIC_PREFIX, s_device_key);

    snprintf(s_topic_cmd, sizeof(s_topic_cmd), APP_MQTT_TOPIC_CMD_FMT, APP_TOPIC_PREFIX, s_device_key);
    snprintf(s_topic_settings, sizeof(s_topic_settings), APP_MQTT_TOPIC_SETTINGS_FMT, APP_TOPIC_PREFIX, s_device_key);
    snprintf(s_topic_cmd_reply, sizeof(s_topic_cmd_reply), APP_MQTT_TOPIC_CMD_GET_RESPONE, APP_TOPIC_PREFIX, s_device_key);

    ESP_LOGI(TAG, "topic telemetry=%s", s_topic_telemetry);
    ESP_LOGI(TAG, "topic cmd=%s", s_topic_cmd);
    ESP_LOGI(TAG, "topic settings=%s", s_topic_settings);
    ESP_LOGI(TAG, "topic reply=%s", s_topic_cmd_reply);

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_initialized = true;
    s_started = false;
    s_connected = false;
    s_connecting = false;
    if (s_lock) xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "broker uri raw=[%s]", s_broker_uri);
    mqtt_debug_dns_from_uri(s_broker_uri);
    return ESP_OK;
}

esp_err_t mqtt_service_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_mqtt_client_handle_t stale = NULL;
    bool stale_started = false;
    bool stale_connected = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_started || s_connecting) {
        if (s_lock) xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    stale = s_mqtt;
    stale_started = s_started;
    stale_connected = s_connected;

    s_mqtt = NULL;
    s_started = false;
    s_connected = false;
    s_connecting = false;

    if (s_lock) xSemaphoreGive(s_lock);

    if (stale) {
        ESP_LOGW(TAG, "stale mqtt client exists, destroy before restart");
        if (stale_started && stale_connected) {
            esp_err_t err = esp_mqtt_client_stop(stale);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "esp_mqtt_client_stop(stale) returned %s", esp_err_to_name(err));
            }
        }
        esp_mqtt_client_destroy(stale);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);

    static char client_id[64];
    snprintf(client_id, sizeof(client_id),
             "dev_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "MQTT start uri=[%s] client_id=[%s] user=[%s]",
             s_broker_uri, client_id, s_username);
    mqtt_debug_dns_from_uri(s_broker_uri);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = s_username,
        .credentials.authentication.password = s_password,
        .credentials.client_id = client_id,
        .session.keepalive = APP_MQTT_KEEPALIVE_SEC,
        .session.last_will.topic = s_topic_status,
        .session.last_will.msg = s_lwt_payload,
        .session.last_will.msg_len = (int)strlen(s_lwt_payload),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = 20000,
        .task.stack_size = 6144,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mqtt = client;
    s_connecting = true;
    s_connected = false;
    s_started = false;
    if (s_lock) xSemaphoreGive(s_lock);

    esp_err_t err = esp_mqtt_client_start(client);
    if (err == ESP_OK) {
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_mqtt == client) {
            s_started = true;
        }
        if (s_lock) xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mqtt == client) {
        s_mqtt = NULL;
        s_started = false;
        s_connected = false;
        s_connecting = false;
    }
    if (s_lock) xSemaphoreGive(s_lock);

    esp_mqtt_client_destroy(client);
    return err;
}

esp_err_t mqtt_service_stop(void)
{
    if (!s_initialized) return ESP_OK;

    esp_mqtt_client_handle_t client = NULL;
    bool was_started = false;
    bool was_connected = false;
    bool was_connecting = false;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    client = s_mqtt;
    was_started = s_started;
    was_connected = s_connected;
    was_connecting = s_connecting;

    ESP_LOGI(TAG, "mqtt stop begin state started=%d connecting=%d connected=%d",
             (int)was_started, (int)was_connecting, (int)was_connected);

    s_mqtt = NULL;
    s_started = false;
    s_connected = false;
    s_connecting = false;

    if (s_lock) xSemaphoreGive(s_lock);

    if (client) {
        if (was_started && was_connected) {
            esp_err_t err = esp_mqtt_client_stop(client);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "esp_mqtt_client_stop returned %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "mqtt stop skip esp_mqtt_client_stop, destroy directly");
        }

        esp_mqtt_client_destroy(client);
    }

    ESP_LOGI(TAG, "mqtt stop destroy done");
    return ESP_OK;
}

int mqtt_service_get_outbox_size(void)
{
    esp_mqtt_client_handle_t client = NULL;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    client = s_mqtt;
    if (s_lock) xSemaphoreGive(s_lock);

    return client ? esp_mqtt_client_get_outbox_size(client) : -1;
}

bool mqtt_service_is_connected(void)
{
    bool v = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_connected;
    if (s_lock) xSemaphoreGive(s_lock);
    return v;
}

bool mqtt_service_is_started(void)
{
    bool v = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    v = (s_started || s_connecting);
    if (s_lock) xSemaphoreGive(s_lock);
    return v;
}