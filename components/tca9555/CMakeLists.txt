idf_component_register(
    SRCS "TCA9555PWR.c"
    INCLUDE_DIRS "include"
     REQUIRES driver esp_driver_i2c freertos
)