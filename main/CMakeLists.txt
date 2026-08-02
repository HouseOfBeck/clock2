idf_component_register(
    SRCS
        "main.c"
        "ethernet.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_uart
        esp_eth
        esp_event
        esp_netif
        esp_timer
)

