idf_component_register(SRCS "time_sync.c" "config_sync.c" "display_manager.c" "telemetry_manager.c" "sensor_fault_manager.c" 
                            "modem_dispatcher.c" "sms_command.c" "alarm_manager.c" "app_logic.c" "config_sync.c"
                        INCLUDE_DIRS "include"
                    REQUIRES ntc_driver esp_event board mqtt_service modem_service diag_service app_events app_config 
                    runtime_config wifi_service provision_service net_manager system_supervisor SHT30 TM1638 power_monitor ds1307 gps_service)
