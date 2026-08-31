# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)

set(SI_HW_REV "rev71" CACHE STRING "Soul Injector hardware revision")
set_property(CACHE SI_HW_REV PROPERTY STRINGS rev3 rev5 rev6 rev71)
if(NOT SI_HW_REV MATCHES "^(rev3|rev5|rev6|rev71)$")
    message(FATAL_ERROR "Unsupported SI_HW_REV '${SI_HW_REV}'. Expected 'rev3', 'rev5', 'rev6', or 'rev71'.")
endif()

if(SI_HW_REV STREQUAL "rev71")
    set(_SI_IDF_TARGET "esp32s31")
else()
    set(_SI_IDF_TARGET "esp32s3")
endif()

if(DEFINED IDF_TARGET AND NOT "${IDF_TARGET}" STREQUAL "${_SI_IDF_TARGET}")
    message(FATAL_ERROR "SI_HW_REV '${SI_HW_REV}' requires IDF_TARGET '${_SI_IDF_TARGET}', not '${IDF_TARGET}'.")
endif()
if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "${_SI_IDF_TARGET}")
    message(FATAL_ERROR "SI_HW_REV '${SI_HW_REV}' requires IDF_TARGET '${_SI_IDF_TARGET}', not '$ENV{IDF_TARGET}'.")
endif()
set(IDF_TARGET "${_SI_IDF_TARGET}" CACHE STRING "ESP-IDF target")

set(_SI_DEFAULT_SDKCONFIG_FILES "sdkconfig.defaults;sdkconfig.defaults.${SI_HW_REV}")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.${SI_HW_REV}" CACHE STRING "Project sdkconfig file")
endif()
message(STATUS "SoulInjector: Hardware revision set to ${SI_HW_REV}")
message(STATUS "SoulInjector: ESP-IDF target is ${_SI_IDF_TARGET}")
message(STATUS "SoulInjector: sdkconfig defaults are ${_SI_DEFAULT_SDKCONFIG_FILES}")
message(STATUS "SoulInjector: sdkconfig output is ${SDKCONFIG}")
if(NOT DEFINED SDKCONFIG_DEFAULTS OR
        SDKCONFIG_DEFAULTS MATCHES "^sdkconfig\\.defaults;(sdkconfig\\.defaults\\.(esp32s3|esp32s31);)?sdkconfig\\.defaults\\.rev(3|5|6|71)$")
    set(SDKCONFIG_DEFAULTS "${_SI_DEFAULT_SDKCONFIG_FILES}" CACHE STRING "SDK config defaults files" FORCE)
endif()

set(EXTRA_COMPONENT_DIRS "external/esp-bsp/components/esp_lvgl_port")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(soulinjector)
