set(SURFER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

idf_component_register(
    SRCS
        app_main.c
        ${SURFER_ROOT}/src/core/rect.c
        ${SURFER_ROOT}/src/core/node.c
        ${SURFER_ROOT}/src/core/compose.c
        ${SURFER_ROOT}/src/core/hit.c
        ${SURFER_ROOT}/src/core/input.c
        ${SURFER_ROOT}/src/widgets/knob.c
        ${SURFER_ROOT}/src/widgets/slider.c
        ${SURFER_ROOT}/src/hal/p4/hal_p4.c
    INCLUDE_DIRS
        .
        ${SURFER_ROOT}/include
        ${SURFER_ROOT}/src/core
        ${SURFER_ROOT}/src/hal/p4
        ${CMAKE_CURRENT_BINARY_DIR}/gen
    PRIV_REQUIRES
        esp_driver_ppa esp_lcd esp_timer esp_mm heap espressif__esp_lcd_touch
)

# widget art baked at build time, same tool as the desktop demo
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${SURFER_ROOT}/tools/gen_widget_assets.py > ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    DEPENDS ${SURFER_ROOT}/tools/gen_widget_assets.py
    VERBATIM
)
add_custom_target(surfer_gen_assets DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h)
add_dependencies(${COMPONENT_LIB} surfer_gen_assets)
