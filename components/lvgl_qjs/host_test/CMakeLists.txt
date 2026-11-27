# Runs the lvgl_qjs binding on a computer: LVGL with a dummy display,
# QuickJS-NG, a fake event loop, and test.js. Built with AddressSanitizer, so
# a use after free or a leaked node fails the run.
#
#   cmake -S components/lvgl_qjs/host_test -B /tmp/lvgl_qjs_host && cmake --build /tmp/lvgl_qjs_host -j
#   /tmp/lvgl_qjs_host/lvgl_qjs_host components/lvgl_qjs/host_test/test.js
cmake_minimum_required(VERSION 3.20)
project(lvgl_qjs_host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(SAN -fsanitize=address,undefined -fno-omit-frame-pointer)

set(SI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(LVGL_ROOT ${SI_ROOT}/components/lvgl)
set(QJS_ROOT ${SI_ROOT}/components/esp-quickjs-ng/quickjs)
set(BINDING ${CMAKE_CURRENT_SOURCE_DIR}/..)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_ROOT}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_host PUBLIC ${LVGL_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl_host PRIVATE -w ${SAN})

add_library(qjs_host STATIC
    ${QJS_ROOT}/quickjs.c ${QJS_ROOT}/libregexp.c ${QJS_ROOT}/libunicode.c
    ${QJS_ROOT}/dtoa.c ${QJS_ROOT}/quickjs-libc.c)
target_include_directories(qjs_host PUBLIC ${QJS_ROOT})
target_compile_definitions(qjs_host PRIVATE _GNU_SOURCE)
target_compile_options(qjs_host PRIVATE -w ${SAN})

add_executable(lvgl_qjs_host main.cpp ${BINDING}/src/lvgl_qjs.cpp ${BINDING}/src/props.cpp)
target_include_directories(lvgl_qjs_host PRIVATE
    ${BINDING}/include ${BINDING}/src ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${SI_ROOT}/components/esp-quickjs-ng/include)
target_link_libraries(lvgl_qjs_host PRIVATE lvgl_host qjs_host m pthread dl)
target_compile_options(lvgl_qjs_host PRIVATE -Wall -Wextra -Wno-missing-field-initializers ${SAN})
target_link_options(lvgl_qjs_host PRIVATE ${SAN})
