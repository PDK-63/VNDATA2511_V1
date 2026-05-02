idf_component_register(
    SRCS "wifi_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif app_events app_config runtime_config nvs_flash
)