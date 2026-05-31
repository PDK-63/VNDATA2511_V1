idf_component_register(SRCS "modem_status.c" "status_led.c" "modem_service.c" INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif lwip driver esp_modem app_config app_events diag_service board)

