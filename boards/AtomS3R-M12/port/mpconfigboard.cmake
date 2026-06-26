set(IDF_TARGET esp32s3)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/AtomS3R-M12/sdkconfig.atoms3r_m12
    boards/AtomS3R-M12/sdkconfig.board
)

# Keep the first bring-up independent of optional MicroPython submodules.
set(MICROPY_PY_BTREE OFF)
