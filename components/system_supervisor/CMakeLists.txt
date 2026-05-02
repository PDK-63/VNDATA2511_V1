idf_component_register(SRCS "system_supervisor.c" 
                        INCLUDE_DIRS "include"
    REQUIRES esp_event freertos modem_service mqtt_service diag_service app_events app_config driver)
