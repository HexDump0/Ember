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
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_timer
)
