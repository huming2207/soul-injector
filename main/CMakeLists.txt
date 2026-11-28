set(SI_MAIN_SRCS
    "main.cpp"
    "prog/swd_prog.cpp" "prog/includes/swd_prog.hpp"
    "prog/fw_asset_manager.cpp" "prog/includes/fw_asset_manager.hpp"
    "prog/asset_store.cpp" "prog/includes/asset_store.hpp"
    "prog/job_controller.cpp" "prog/includes/job_controller.hpp"
    "prog/prog_log.cpp" "prog/includes/prog_log.hpp"
    "prog/offline_flasher.cpp" "prog/includes/offline_flasher.hpp"
    "prog/target_backend.hpp"
    "prog/swd_cortexm_backend.cpp" "prog/swd_cortexm_backend.hpp"
    "prog/sidp_swd_backend.cpp" "prog/sidp_swd_backend.hpp"
    "prog/target_lines.cpp" "prog/includes/target_lines.hpp"
    "prog/esp32_serial_backend.cpp" "prog/esp32_serial_backend.hpp"
    "prog/config/target_config.hpp"
    "prog/config/job_decoder.cpp" "prog/config/job_decoder.hpp"
    "prog/includes/led_ctrl.hpp"
    "prog/includes/current_tester.hpp"
    "prog/procedure_executor.cpp" "prog/includes/procedure_executor.hpp"
    "comm/wifi_manager.cpp" "comm/wifi_manager.hpp"
    "comm/net_config.cpp" "comm/net_config.hpp"
    "comm/modem_manager.cpp" "comm/modem_manager.hpp"
    "comm/quectel_dte.cpp" "comm/quectel_dte.hpp"
    "comm/sidp_service.cpp" "comm/sidp_service.hpp"
    "comm/agent_service.cpp" "comm/agent_service.hpp"
    "comm/incident_store.cpp" "comm/incident_store.hpp"
    "power/power_profiler.cpp" "power/power_profiler.hpp"
    "bootstrap_fsm.cpp" "bootstrap_fsm.hpp"
    "driver/lcd/display_manager.cpp" "driver/lcd/display_manager.hpp"
    "ui/ui_composer_noop.cpp" "ui/ui_composer_noop.hpp" "ui/si_ui.hpp"
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
        "driver/lcd/nt279vj_panel.cpp" "driver/lcd/nt279vj_panel.hpp"
        "ui/ui_composer_279.cpp" "ui/ui_composer_279.hpp"
        "ui/ui_view.cpp" "ui/ui_view.hpp"
        "ui/ui_screens.cpp" "ui/ui_model.hpp"
    )
endif()

# esp_modem's UART terminal creator lives in the component's private headers
idf_component_get_property(ESP_MODEM_DIR espressif__esp_modem COMPONENT_DIR)

idf_component_register(
    SRCS
        ${SI_MAIN_SRCS}

    INCLUDE_DIRS
        "."
        "prog"
        "prog/includes"
        "prog/config"
        "comm"
        "power"
        "driver" "driver/lcd"
        "external"
        "ui"
        "${ESP_MODEM_DIR}/private_include"

    PRIV_REQUIRES
        "swd_esp"
        "sidp_client"
        "on9ringstore"
        "mbedtls"
        "usb"
        "spi_flash"
        "efuse"
        "nvs_flash"
        "protocomm"
        "protobuf-c"
        "driver"
        "esp_timer"
        "lvgl"
        "led_strip"
        "arduino_json"
        "esp_lcd"
        "wear_levelling"
        "fatfs"
        "esp_app_format"
        "esp_wifi"
        "esp-mqtt"
        "soulinjector-common"
        "app_update"
        "ina228"
        "ina226"
        "esp_lvgl_port"
        "esp-serial-flasher"
        "qjs_binder"

    REQUIRES
        "tinyusb"
        "esp_tinyusb"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-deprecated-enum-enum-conversion" "-fpermissive")

# si_build_info.h: the commit the firmware is built from. A target that always
# runs, so the header follows commits and edits made after configure; the
# script leaves the file alone when nothing changed.
set(SI_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/si_build_info")
set(SI_BUILD_INFO_H "${SI_BUILD_INFO_DIR}/si_build_info.h")
idf_build_get_property(SI_PROJECT_DIR PROJECT_DIR)
file(MAKE_DIRECTORY "${SI_BUILD_INFO_DIR}")
add_custom_target(si_build_info
    COMMAND ${CMAKE_COMMAND} -D "SRC_DIR=${SI_PROJECT_DIR}" -D "OUT=${SI_BUILD_INFO_H}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/build_info.cmake"
    BYPRODUCTS "${SI_BUILD_INFO_H}"
    COMMENT "Reading the firmware commit"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} si_build_info)
target_include_directories(${COMPONENT_LIB} PRIVATE "${SI_BUILD_INFO_DIR}")
