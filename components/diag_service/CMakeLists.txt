idf_component_register(SRCS "diag_service.c" 
                        INCLUDE_DIRS "include" 
                        REQUIRES esp_system esp_timer nvs_flash app_config driver board app_config
                        )
