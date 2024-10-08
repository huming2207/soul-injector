file(GLOB_RECURSE MPY_SRC "mpy_embed/*.c")
file(GLOB_RECURSE MPY_MOD_SRC "mods/*.c")
file(GLOB_RECURSE MPY_GLUE_SRC "glue/*.c" "glue/*.cpp" "glue/*.h" "glue/*.hpp")

idf_component_register(
        SRCS ${MPY_SRC} ${MPY_MOD_SRC} ${MPY_GLUE_SRC}
        INCLUDE_DIRS
            "mpy_embed"
            "mpy_embed/micropython_embed" "mpy_embed/build-embed" "mpy_embed/build-embed/genhdr"
            "glue"
)

set(MPY_TOP ${COMPONENT_DIR}/micropython)
add_custom_command(OUTPUT "mpy_embed"
        COMMAND make -C ${COMPONENT_DIR}/mpy_embed MICROPYTHON_TOP=${MPY_TOP} -f ${COMPONENT_DIR}/mpy_embed/micropython_embed.mk
        DEPENDS ${COMPONENT_DIR}/mpy_embed/mpconfigport.h
        VERBATIM)

add_custom_target(mpy DEPENDS "mpy_embed")
add_dependencies(${COMPONENT_LIB} mpy)

set_property(DIRECTORY "${COMPONENT_DIR}" APPEND PROPERTY
        ADDITIONAL_CLEAN_FILES "mpy_embed/build-embed" "mpy_embed/micropython_embed")