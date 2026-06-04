set(IDF_TARGET esp32s31)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.s31_korvo
    boards/ESP32_S31_KORVO/sdkconfig.board
)

set(MICROPY_PY_BTREE OFF)
