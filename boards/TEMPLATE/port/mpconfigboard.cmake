# Copy the parent board directory (boards/TEMPLATE) to boards/<BOARD> and
# replace TEMPLATE settings before building a real board. The build projects
# boards/<BOARD>/port/ onto the MicroPython esp32 port, so the SDKCONFIG_DEFAULTS
# paths below are resolved relative to ports/esp32.

set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/TEMPLATE/sdkconfig.p4x_template
    boards/TEMPLATE/sdkconfig.board
)

# Keep early bring-up independent of optional MicroPython submodules.
set(MICROPY_PY_BTREE OFF)
