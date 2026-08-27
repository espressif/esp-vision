set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/ESP32_P4X_VISION/sdkconfig.p4x_vision
    boards/ESP32_P4X_VISION/sdkconfig.defaults.board
    boards/ESP32_P4X_VISION/sdkconfig.board
)

# Keep early bring-up independent of optional MicroPython submodules.
set(MICROPY_PY_BTREE OFF)
