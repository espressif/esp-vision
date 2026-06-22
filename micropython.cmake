# ESP-VISION MicroPython user C module entry point.
#
# Keep this file valid even before platform/modules sources exist so the
# baseline MicroPython firmware can be built early in the porting process.

set(ESP_VISION_BOARD "ESP32_P4X_EYE")
if(DEFINED MICROPY_BOARD)
    set(ESP_VISION_BOARD "${MICROPY_BOARD}")
endif()

set(ESP_VISION_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(ESP_VISION_BOARD_DIR "${ESP_VISION_ROOT}/boards/${ESP_VISION_BOARD}")
set(MICROPY_MANIFEST_ESP_VISION_ROOT "${ESP_VISION_ROOT}")


set(ESP_VISION_ENABLE_BARCODE OFF)
if(EXISTS "${ESP_VISION_BOARD_DIR}/board.cmake")
    include("${ESP_VISION_BOARD_DIR}/board.cmake")
endif()

list(APPEND MICROPY_QSTRDEFS_PORT
    ${ESP_VISION_ROOT}/modules/qstrdefs_esp_vision.h
)

set(MICROPY_ESP32_MAIN_SOURCE "${ESP_VISION_ROOT}/platform/main.c")

set(ESP_VISION_MODULE_SOURCES
    ${ESP_VISION_ROOT}/modules/py_display.c
    ${ESP_VISION_ROOT}/modules/py_image.c
    ${ESP_VISION_ROOT}/modules/py_imageio.c
    ${ESP_VISION_ROOT}/modules/py_helper.c
    ${ESP_VISION_ROOT}/modules/py_sensor.c
)

if((IDF_TARGET STREQUAL "esp32p4") OR (IDF_TARGET STREQUAL "esp32s3") OR (IDF_TARGET STREQUAL "esp32s31"))
    list(APPEND ESP_VISION_MODULE_SOURCES
        ${ESP_VISION_ROOT}/modules/py_espdl.cpp
        ${ESP_VISION_ROOT}/modules/py_tflite.cpp
    )
endif()

# The hardware H.264 encoder (esp_h264) and RTSP server (esp_media_protocols) are P4-only.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND ESP_VISION_MODULE_SOURCES
        ${ESP_VISION_ROOT}/platform/h264.c
        ${ESP_VISION_ROOT}/modules/py_h264.c
        ${ESP_VISION_ROOT}/modules/py_rtsp.c
    )
endif()

set(ESP_VISION_CAMERA_SOURCE "${ESP_VISION_ROOT}/platform/camera.c")
set(ESP_VISION_BOARD_SOURCES)
if(EXISTS "${ESP_VISION_BOARD_DIR}/camera.c")
    set(ESP_VISION_CAMERA_SOURCE "${ESP_VISION_BOARD_DIR}/camera.c")
endif()

foreach(source sdcard.c display.c)
    if(EXISTS "${ESP_VISION_BOARD_DIR}/${source}")
        list(APPEND ESP_VISION_BOARD_SOURCES "${ESP_VISION_BOARD_DIR}/${source}")
    endif()
endforeach()

add_library(usermod_esp_vision_platform INTERFACE)

target_sources(usermod_esp_vision_platform INTERFACE
    ${ESP_VISION_MODULE_SOURCES}
    ${ESP_VISION_CAMERA_SOURCE}
    ${ESP_VISION_ROOT}/platform/debug.c
    ${ESP_VISION_ROOT}/platform/display.c
    ${ESP_VISION_ROOT}/platform/jpeg.c
    ${ESP_VISION_ROOT}/platform/preview.c
    ${ESP_VISION_ROOT}/platform/sdcard.c
    ${ESP_VISION_ROOT}/platform/usb_msc.c
    ${ESP_VISION_BOARD_SOURCES}
)

target_include_directories(usermod_esp_vision_platform INTERFACE
    ${ESP_VISION_ROOT}/modules
    ${ESP_VISION_ROOT}/platform
    ${ESP_VISION_BOARD_DIR}
    ${ESP_VISION_ROOT}/lib
    ${ESP_VISION_ROOT}/components/imlib/upstream
    ${ESP_VISION_ROOT}/components/imlib/include
    ${CMAKE_BINARY_DIR}
)

target_compile_definitions(usermod_esp_vision_platform INTERFACE
    CMSIS_MCU_H="cmsis_compiler.h"
    OMV_NO_GPL=1
)

# Barcode support is opt-in per board (board sets ESP_VISION_ENABLE_BARCODE in
# its boards/<board>/board.cmake) and requires the zxing-cpp submodule. Keep
# this condition in sync with components/zxing/CMakeLists.txt.
if(ESP_VISION_ENABLE_BARCODE
        AND EXISTS "${ESP_VISION_ROOT}/lib/zxing-cpp/core/CMakeLists.txt")
    target_compile_definitions(usermod_esp_vision_platform INTERFACE
        ESP_VISION_ENABLE_ZXING_1D=1
    )

    target_include_directories(usermod_esp_vision_platform INTERFACE
        ${ESP_VISION_ROOT}/components/zxing/include
    )

    target_link_libraries(usermod_esp_vision_platform INTERFACE idf::zxing)
endif()

target_compile_options(usermod_esp_vision_platform INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>
)

target_link_libraries(usermod INTERFACE usermod_esp_vision_platform)

include(${CMAKE_CURRENT_LIST_DIR}/lib/ulab/code/micropython.cmake)
