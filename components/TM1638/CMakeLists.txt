idf_component_register(
    SRCS "tm1638_4G_ui.c" "tm1638_server_ui.c" "tm1638_wifi_ui.c" "tm1638.c" 
    INCLUDE_DIRS "include"
     REQUIRES driver
)