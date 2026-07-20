# surfer user C module for the MicroPython esp32 port (ESP32-P4).
# Build from the surfer repo root with:  make mpy-p4
# (generates asset headers first, then builds MP with BOARD_DIR=SURFER_P4)

set(SURFER_MOD_DIR ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(SURFER_DIR ${SURFER_MOD_DIR}/../.. ABSOLUTE)

add_library(usermod_surfer INTERFACE)

target_sources(usermod_surfer INTERFACE
    ${SURFER_MOD_DIR}/modsurfer.c
    ${SURFER_MOD_DIR}/port_p4.c
    ${SURFER_MOD_DIR}/usb_kbd.c
    ${SURFER_DIR}/src/core/rect.c
    ${SURFER_DIR}/src/core/node.c
    ${SURFER_DIR}/src/core/image.c
    ${SURFER_DIR}/src/core/shape.c
    ${SURFER_DIR}/src/core/pad.c
    ${SURFER_DIR}/src/core/compose.c
    ${SURFER_DIR}/src/core/hit.c
    ${SURFER_DIR}/src/core/input.c
    ${SURFER_DIR}/src/core/scroll.c
    ${SURFER_DIR}/src/text/font.c
    ${SURFER_DIR}/src/text/label.c
    ${SURFER_DIR}/src/text/textinput.c
    ${SURFER_DIR}/src/text/textgrid.c
    ${SURFER_DIR}/src/widgets/knob.c
    ${SURFER_DIR}/src/widgets/slider.c
    ${SURFER_DIR}/src/widgets/checkbox.c
    ${SURFER_DIR}/src/widgets/dropdown.c
    ${SURFER_DIR}/src/widgets/button.c
    ${SURFER_DIR}/src/hal/p4/hal_p4.c
)

target_include_directories(usermod_surfer INTERFACE
    ${SURFER_MOD_DIR}
    ${SURFER_DIR}/include
    ${SURFER_DIR}/src/core
    ${SURFER_DIR}/src/hal/p4
    ${SURFER_DIR}/build/gen
    ${SURFER_DIR}/tools
    # esp_async_fbcpy (DMA2D rect copy) ships in esp_lcd's priv_include
    $ENV{IDF_PATH}/components/esp_lcd/priv_include
)

target_link_libraries(usermod_surfer INTERFACE
    idf::esp_driver_ppa
    idf::esp_lcd
    idf::esp_driver_i2c
    idf::usb
)

target_link_libraries(usermod INTERFACE usermod_surfer)
