idf_component_register(SRCS "main.c"
    REQUIRES nvs_flash esp_event esp_netif board diag_service app_events modem_service mqtt_service app_logic system_supervisor runtime_config ota_service
    provision_service net_manager wifi_service legacy_ec800 SHT30 TM1638 ntc_driver esp_modem power_monitor ethernet_service ds1307 sd_logger
    INCLUDE_DIRS ".")