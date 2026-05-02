idf_component_register(SRCS "ota_service.c" INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client esp_event mbedtls app_events esp-tls
    runtime_config nvs_flash app_config)
