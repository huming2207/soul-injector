idf_component_register(
        SRCS
        "ina226.cpp" "ina226.hpp"

        INCLUDE_DIRS
        "."

        REQUIRES
        "driver" "esp_event" "esp_driver_gpio" "esp_driver_i2c"
)