idf_component_register(SRCS "main.cpp" PRIV_REQUIRES qjs_binder esp_timer)

idf_build_get_property(python PYTHON)
set(tool "${CMAKE_CURRENT_LIST_DIR}/../../../esp-quickjs-ng/tools/qjs_esp_bc.py")
set(qbc "${CMAKE_CURRENT_BINARY_DIR}/test.qbc")
add_custom_command(OUTPUT "${qbc}"
    COMMAND "${python}" "${tool}" compile "${CMAKE_CURRENT_LIST_DIR}/test.js" -o "${qbc}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test.js" "${tool}"
    VERBATIM)
add_custom_target(test_qbc DEPENDS "${qbc}")
add_dependencies(${COMPONENT_LIB} test_qbc)
target_add_binary_data(${COMPONENT_LIB} "${qbc}" BINARY)

set(stm_js "${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/stm32wl-rdp.js")
set(stm_qbc "${CMAKE_CURRENT_BINARY_DIR}/stm32wl.qbc")
add_custom_command(OUTPUT "${stm_qbc}"
    COMMAND "${python}" "${tool}" compile "${stm_js}" -o "${stm_qbc}"
    DEPENDS "${stm_js}" "${tool}"
    VERBATIM)
add_custom_target(stm_qbc DEPENDS "${stm_qbc}")
add_dependencies(${COMPONENT_LIB} stm_qbc)
target_add_binary_data(${COMPONENT_LIB} "${stm_qbc}" BINARY)

set(sess_qbc "${CMAKE_CURRENT_BINARY_DIR}/session.bin")
add_custom_command(OUTPUT "${sess_qbc}"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/session_bc.py" "${tool}" "${CMAKE_CURRENT_LIST_DIR}/session.js" "${sess_qbc}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/session.js" "${CMAKE_CURRENT_LIST_DIR}/session_bc.py" "${tool}"
    VERBATIM)
add_custom_target(sess_qbc DEPENDS "${sess_qbc}")
add_dependencies(${COMPONENT_LIB} sess_qbc)
target_add_binary_data(${COMPONENT_LIB} "${sess_qbc}" BINARY)
