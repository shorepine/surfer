set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4
    ${MICROPY_BOARD_DIR}/sdkconfig.surfer
)

set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)
