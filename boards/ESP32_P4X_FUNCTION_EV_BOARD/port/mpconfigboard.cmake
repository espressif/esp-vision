set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4_wifi_common
    boards/ESP32_P4X_FUNCTION_EV_BOARD/sdkconfig.p4x_function_ev_board
    boards/ESP32_P4X_FUNCTION_EV_BOARD/sdkconfig.board
)

# Keep the first bring-up independent of optional MicroPython submodules.
set(MICROPY_PY_BTREE OFF)
