idf_component_register(
    SRCS "src/js_host.cpp" "src/mod_target.cpp" "src/mod_misc.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp-quickjs-ng mbedtls esp_timer
)
