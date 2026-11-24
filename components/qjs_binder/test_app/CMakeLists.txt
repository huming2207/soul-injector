# qjs_binder against a mock target, in esp-emu. Prints "BINDER PASS" / "BINDER FAIL".
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS ".." "../../esp-quickjs-ng")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qjs_binder_test)
