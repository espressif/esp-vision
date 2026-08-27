set(IDF_TARGET esp32s31)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/ESP32_S31_MOSAICO/sdkconfig.s31_mosaico
    boards/ESP32_S31_MOSAICO/sdkconfig.defaults.board
    boards/ESP32_S31_MOSAICO/sdkconfig.board
)

set(MICROPY_PY_BTREE OFF)

# board_variant.c reads BLOCK_USR_DATA during cold startup. Declare efuse here
# so ESP-IDF sees the dependency during its early component-discovery pass.
list(APPEND IDF_COMPONENTS efuse)
