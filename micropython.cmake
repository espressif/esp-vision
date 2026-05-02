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

set(MICROPY_ESP32_MAIN_SOURCE "${ESP_VISION_ROOT}/platform/main.c")

set(ESP_VISION_BOARD_SOURCES)
foreach(source sdcard.c)
    if(EXISTS "${ESP_VISION_BOARD_DIR}/${source}")
        list(APPEND ESP_VISION_BOARD_SOURCES "${ESP_VISION_BOARD_DIR}/${source}")
    endif()
endforeach()

add_library(usermod_esp_vision_platform INTERFACE)

target_sources(usermod_esp_vision_platform INTERFACE
    ${ESP_VISION_ROOT}/modules/py_image.c
    ${ESP_VISION_ROOT}/modules/py_helper.c
    ${ESP_VISION_ROOT}/modules/py_sensor.c
    ${ESP_VISION_ROOT}/platform/camera.c
    ${ESP_VISION_ROOT}/platform/debug.c
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

target_link_libraries(usermod INTERFACE usermod_esp_vision_platform)

include(${CMAKE_CURRENT_LIST_DIR}/lib/ulab/code/micropython.cmake)
