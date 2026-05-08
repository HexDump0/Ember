if(CONFIG_EMBER_FAKE_SENSORS)
    set(SENSORS_SRC "sensors_fake.c")
else()
    set(SENSORS_SRC "sensors.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "${SENSORS_SRC}"
        "logger.c"
        "wifi_ap.c"
        "http_server.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        nvs_flash
    EMBED_FILES
        "web/index.html"
)
