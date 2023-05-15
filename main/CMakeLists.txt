idf_component_register(
        SRCS
            "main.cpp"
            "prog/swd_prog.cpp" "prog/includes/swd_prog.hpp"
            "prog/config_manager.cpp" "prog/includes/config_manager.hpp"
            "prog/swd_headless_flasher.cpp" "prog/includes/swd_headless_flasher.hpp"
            "external/includes/ArduinoJson.hpp"
            "external/includes/lfbb.hpp" "external/includes/lfbb_impl.hpp"
            "prog/includes/led_ctrl.hpp"
            "misc/includes/file_utils.hpp"
            "comm/usb/cdc_acm.cpp" "comm/usb/cdc_acm.hpp"
            "comm/comm_fsm.cpp" "comm/comm_fsm.hpp"
            "comm/comm_interface.hpp"
            "misc/includes/soul_nvs.hpp"
            "driver/includes/button_monitor.hpp"
            "driver/lcd/lhs154kc.c" "driver/lcd/lhs154kc.h"
            "driver/lcd/lvgl_wrapper.c" "driver/lcd/lvgl_wrapper.h"
            "driver/lcd/lcd_manager.cpp" "driver/lcd/lcd_manager.hpp"

        INCLUDE_DIRS
            "." "misc/includes" "prog/includes" "external/includes" "comm" "comm/usb" "driver/includes" "driver"
        PRIV_REQUIRES
            "daplink-esp"
            "mbedtls"
            "esp_littlefs"
            "usb"
            "spi_flash"
            "efuse"
            "nvs_flash"
            "driver"
            "esp_timer"
            "lvgl"
            "led_strip"
)
