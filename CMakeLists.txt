# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)

set(SI_HW_REV "rev5" CACHE STRING "Soul Injector hardware revision")
set_property(CACHE SI_HW_REV PROPERTY STRINGS rev3 rev5)
if(NOT SI_HW_REV MATCHES "^(rev3|rev5)$")
    message(FATAL_ERROR "Unsupported SI_HW_REV '${SI_HW_REV}'. Expected 'rev3' or 'rev5'.")
endif()

set(_SI_DEFAULT_SDKCONFIG_FILES "sdkconfig.defaults;sdkconfig.defaults.${SI_HW_REV}")
message(STATUS "SoulInjector: Hardware revision set to ${SI_HW_REV}")
message(STATUS "SoulInjector: sdkconfig is ${_SI_DEFAULT_SDKCONFIG_FILES}")
if(NOT DEFINED SDKCONFIG_DEFAULTS OR SDKCONFIG_DEFAULTS MATCHES "^sdkconfig\\.defaults;sdkconfig\\.defaults\\.rev[35]$")
    set(SDKCONFIG_DEFAULTS "${_SI_DEFAULT_SDKCONFIG_FILES}" CACHE STRING "SDK config defaults files" FORCE)
endif()

set(EXTRA_COMPONENT_DIRS "external/esp-bsp/components/esp_lvgl_port")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(soulinjector)
