idf_component_register(
    SRCS "src/lvgl_qjs.cpp" "src/props.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl esp-quickjs-ng
)
