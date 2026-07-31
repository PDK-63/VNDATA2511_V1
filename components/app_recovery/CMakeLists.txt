idf_component_register(
    SRCS "app_recovery.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_events
        runtime_config
        mqtt_service
        modem_service
        esp_event
        esp_wifi
)