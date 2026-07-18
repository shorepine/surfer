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
        ${SURFER_ROOT}/src/core/scroll.c
        ${SURFER_ROOT}/src/text/font.c
        ${SURFER_ROOT}/src/text/label.c
        ${SURFER_ROOT}/src/text/textinput.c
        ${SURFER_ROOT}/src/text/textgrid.c
        ${SURFER_ROOT}/src/widgets/knob.c
        ${SURFER_ROOT}/src/widgets/slider.c
        ${SURFER_ROOT}/src/widgets/checkbox.c
        ${SURFER_ROOT}/src/widgets/dropdown.c
        ${SURFER_ROOT}/src/hal/p4/hal_p4.c
    INCLUDE_DIRS
        .
        ${SURFER_ROOT}/include
        ${SURFER_ROOT}/src/core
        ${SURFER_ROOT}/src/hal/p4
        ${CMAKE_CURRENT_BINARY_DIR}/gen
        # esp_async_fbcpy (DMA2D rect copy) ships in esp_lcd's priv_include
        $ENV{IDF_PATH}/components/esp_lcd/priv_include
    PRIV_REQUIRES
        esp_driver_ppa esp_lcd esp_timer esp_mm heap espressif__esp_lcd_touch
)

# widget art + fonts baked at build time, same tools as the desktop demo.
# fontbake is a host tool: built with the host cc, not the cross compiler.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${SURFER_ROOT}/tools/gen_widget_assets.py > ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    DEPENDS ${SURFER_ROOT}/tools/gen_widget_assets.py
    VERBATIM
)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND cc -O2 -I${SURFER_ROOT}/tools -o ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake
            ${SURFER_ROOT}/tools/fontbake.c -lm
    DEPENDS ${SURFER_ROOT}/tools/fontbake.c
    VERBATIM
)
set(ROBOTO ${SURFER_ROOT}/assets/fonts/Roboto-Regular.ttf)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui16.h
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ui16 16 ${ROBOTO}
            ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui16.h
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ${ROBOTO}
    VERBATIM
)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui28.h
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ui28 28 ${ROBOTO}
            ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui28.h
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/fontbake ${ROBOTO}
    VERBATIM
)
add_custom_target(surfer_gen_assets DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/gen/widget_assets.h
    ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui16.h
    ${CMAKE_CURRENT_BINARY_DIR}/gen/font_ui28.h)
add_dependencies(${COMPONENT_LIB} surfer_gen_assets)
