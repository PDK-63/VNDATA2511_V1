idf_component_register(
    SRCS
        "esp_eth_mac_w5500.c"
        "esp_eth_phy_w5500.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_eth driver esp_timer W5500
)