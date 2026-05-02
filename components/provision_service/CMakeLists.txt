idf_component_register(
    SRCS "provision_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server runtime_config app_events nvs_flash esp_system mqtt_service app_config
)