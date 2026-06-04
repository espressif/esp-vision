# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: MIT

set(ESP_VISION_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(IMLIB_MICROPY_DIR "${ESP_VISION_ROOT}/lib/micropython")
set(IMLIB_MICROPY_PORT_DIR "${IMLIB_MICROPY_DIR}/ports/esp32")
set(IMLIB_BOARD "ESP32_P4X_EYE")

if(DEFINED MICROPY_BOARD)
    set(IMLIB_BOARD "${MICROPY_BOARD}")
endif()

set(IMLIB_ESP_VISION_BOARD_DIR "${ESP_VISION_ROOT}/boards/${IMLIB_BOARD}")

if(DEFINED MICROPY_BOARD_DIR)
    set(IMLIB_MICROPY_BOARD_DIR "${MICROPY_BOARD_DIR}")
elseif(DEFINED MICROPY_BOARD)
    set(IMLIB_MICROPY_BOARD_DIR "${IMLIB_MICROPY_PORT_DIR}/boards/${MICROPY_BOARD}")
else()
    set(IMLIB_MICROPY_BOARD_DIR "${IMLIB_MICROPY_PORT_DIR}/boards/ESP32_P4X_EYE")
endif()

idf_component_register(
    SRCS
        "upstream/collections.c"
        "upstream/apriltag.c"
        "upstream/binary.c"
        "upstream/bayer.c"
        "upstream/blob.c"
        "upstream/bmp.c"
        "upstream/draw.c"
        "upstream/edge.c"
        "upstream/eye.c"
        "upstream/filter.c"
        "upstream/fmath.c"
        "upstream/font.c"
        "upstream/fsort.c"
        "upstream/hough.c"
        "upstream/imlib.c"
        "upstream/integral.c"
        "upstream/jpegd.c"
        "upstream/jpege.c"
        "upstream/line.c"
        "upstream/mathop.c"
        "upstream/png.c"
        "upstream/point.c"
        "upstream/ppm.c"
        "upstream/qrcode.c"
        "upstream/rectangle.c"
        "upstream/stats.c"
        "upstream/template.c"
        "upstream/yuv.c"
        "compat/array.c"
        "compat/fb_alloc.c"
        "compat/file_utils.c"
        "compat/framebuffer_compat.c"
        "compat/lab_table.c"
        "compat/sincos_table.c"
        "compat/unaligned_memcpy.c"
        "compat/umm_malloc.c"
    INCLUDE_DIRS
        "upstream"
        "${IMLIB_ESP_VISION_BOARD_DIR}"
        "include"
        "${IMLIB_MICROPY_DIR}"
        "${IMLIB_MICROPY_PORT_DIR}"
        "${IMLIB_MICROPY_BOARD_DIR}"
        "${CMAKE_BINARY_DIR}"
)

set(IMLIB_MICROPY_GENHDRS
    "${CMAKE_BINARY_DIR}/genhdr/qstrdefs.generated.h"
    "${CMAKE_BINARY_DIR}/genhdr/compressed.data.h"
)

add_custom_target(imlib_micropython_genhdr DEPENDS ${IMLIB_MICROPY_GENHDRS})
add_dependencies(${COMPONENT_LIB} imlib_micropython_genhdr)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CMSIS_MCU_H="cmsis_compiler.h"
    OMV_NO_GPL=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-error
)

if($ENV{IDF_VERSION} VERSION_GREATER_EQUAL "6.0")
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
        -Wno-uninitialized
        -Wno-maybe-uninitialized)
endif()
