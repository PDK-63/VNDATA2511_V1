idf_component_register(SRCS "mqtt_service.c" INCLUDE_DIRS "include"
    REQUIRES mqtt esp_event esp_timer app_config app_events diag_service ota_service runtime_config esp-tls json)
