set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/ESP32_VISION_P4X/sdkconfig.vision_p4x
    boards/ESP32_VISION_P4X/sdkconfig.board
)

# Keep early bring-up independent of optional MicroPython submodules.
set(MICROPY_PY_BTREE OFF)
