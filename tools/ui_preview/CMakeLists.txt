# Renders every screen of the 2.79" UI to PNG on a computer, from the same
# main/ui sources the firmware uses. See README.md.
cmake_minimum_required(VERSION 3.20)
project(ui_preview C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

set(SI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LVGL_ROOT ${SI_ROOT}/components/lvgl)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_ROOT}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_host PUBLIC ${LVGL_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl_host PRIVATE -w)

add_executable(ui_preview
    main.cpp
    ${SI_ROOT}/main/ui/ui_screens.cpp
    ${SI_ROOT}/main/ui/ui_view.cpp
)
target_include_directories(ui_preview PRIVATE ${SI_ROOT}/main/ui)
target_link_libraries(ui_preview PRIVATE lvgl_host)
target_compile_options(ui_preview PRIVATE -Wall -Wextra)
