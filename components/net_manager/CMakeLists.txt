idf_component_register(
    SRCS "net_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES runtime_config wifi_service provision_service modem_service app_events esp_event app_config 
    mqtt_service system_supervisor TM1638 ethernet_service
) 