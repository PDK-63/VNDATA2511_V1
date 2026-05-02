idf_component_register(
    SRCS "ethernet_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif esp_eth driver app_events app_config W5500
)