set(SI_MAIN_SRCS
    "main.cpp"
    "prog/swd_prog.cpp" "prog/includes/swd_prog.hpp"
    "prog/fw_asset_manager.cpp" "prog/includes/fw_asset_manager.hpp"
    "prog/offline_flasher.cpp" "prog/includes/offline_flasher.hpp"
    "prog/includes/led_ctrl.hpp"
    "prog/includes/current_tester.hpp"
    "prog/procedure_executor.cpp" "prog/includes/procedure_executor.hpp"
    "comm/wifi_manager.cpp" "comm/wifi_manager.hpp"
    "bootstrap_fsm.cpp" "bootstrap_fsm.hpp"
    "driver/lcd/display_manager.cpp" "driver/lcd/display_manager.hpp"
    "ui/ui_composer_noop.cpp" "ui/ui_composer_noop.hpp"
)

if(CONFIG_SI_SG_PROG_RIG)
    list(APPEND SI_MAIN_SRCS
        "prog/current_tester.cpp"
    )
endif()

if(CONFIG_SI_DISP_ENABLE)
    list(APPEND SI_MAIN_SRCS
        "driver/lcd/esp_lcd_panel_nv3007.c" "driver/lcd/esp_lcd_panel_nv3007.h"
        "driver/lcd/lhs154kc_panel.cpp" "driver/lcd/lhs154kc_panel.hpp"
        "driver/lcd/nfp114h_panel.cpp" "driver/lcd/nfp114h_panel.hpp"
        "driver/lcd/nt279vj_panel.cpp" "driver/lcd/nt279vj_panel.hpp"
        "ui/ui_composer_114.cpp" "ui/ui_composer_114.hpp"
        "ui/ui_screen_progress_114.cpp" "ui/ui_screen_progress_114.hpp"
        "ui/ui_screen_message_114.cpp" "ui/ui_screen_message_114.hpp"
        "ui/ui_screen_current_114.cpp" "ui/ui_screen_current_114.hpp"
        "ui/ui_composer_279.cpp" "ui/ui_composer_279.hpp"
        "ui/ui_screen_progress_279.cpp" "ui/ui_screen_progress_279.hpp"
        "ui/ui_screen_message_279.cpp" "ui/ui_screen_message_279.hpp"
        "ui/ui_screen_current_279.cpp" "ui/ui_screen_current_279.hpp"
    )
endif()

idf_component_register(
    SRCS
        ${SI_MAIN_SRCS}

    INCLUDE_DIRS
        "."
        "prog/includes"
        "comm"
        "driver" "driver/lcd"
        "external"
        "ui"

    PRIV_REQUIRES
        "swd_esp"
        "mbedtls"
        "usb"
        "spi_flash"
        "efuse"
        "nvs_flash"
        "driver"
        "esp_timer"
        "lvgl"
        "led_strip"
        "arduino_json"
        "esp_lcd"
        "wear_levelling"
        "esp_wifi"
        "esp-mqtt"
        "mbedtls"
        "soulinjector-common"
        "app_update"
        "ina228"
        "esp_lvgl_port"
        "spi_flash"

    REQUIRES
        "tinyusb"
        "esp_tinyusb"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-deprecated-enum-enum-conversion" "-fpermissive")
