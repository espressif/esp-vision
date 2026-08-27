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

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND ESP_VISION_MODULE_SOURCES
        ${ESP_VISION_ROOT}/platform/usb_auto_download.c
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

set(ESP_VISION_BOARD_MANAGER_SOURCES)
set(ESP_VISION_BOARD_MANAGER_INCLUDE_DIRS)
if(ESP_VISION_USE_BOARD_MANAGER)
    idf_component_get_property(
        ESP_VISION_BOARD_MANAGER_DIR
        espressif__esp_board_manager
        COMPONENT_DIR
    )
    idf_component_get_property(
        ESP_VISION_BOARDS_DIR
        espressif__esp_boards
        COMPONENT_DIR
    )
    idf_build_get_property(ESP_VISION_PYTHON PYTHON)

    set(ESP_VISION_BOARD_MANAGER_INPUT_DIR "${ESP_VISION_BOARD_DIR}/bmgr")
    set(ESP_VISION_BOARD_MANAGER_BOARD_DIR "${ESP_VISION_BOARD_MANAGER_INPUT_DIR}")
    set(ESP_VISION_BOARD_MANAGER_CUSTOMER_PATH "${ESP_VISION_BOARD_MANAGER_INPUT_DIR}")
    set(ESP_VISION_BOARD_MANAGER_AMEND_ARGS)
    if(ESP_VISION_BOARD_MANAGER_ESP_BOARDS_BOARD)
        set(ESP_VISION_BOARD_MANAGER_BOARD_DIR
            "${ESP_VISION_BOARDS_DIR}/${ESP_VISION_BOARD_MANAGER_ESP_BOARDS_BOARD}")
        set(ESP_VISION_BOARD_MANAGER_CUSTOMER_PATH "${ESP_VISION_BOARDS_DIR}")
        list(APPEND ESP_VISION_BOARD_MANAGER_AMEND_ARGS
            --amend "${ESP_VISION_BOARD_MANAGER_INPUT_DIR}")
        if(NOT EXISTS "${ESP_VISION_BOARD_MANAGER_BOARD_DIR}/board_info.yaml")
            message(FATAL_ERROR
                "ESP Board Manager board '${ESP_VISION_BOARD_MANAGER_ESP_BOARDS_BOARD}' "
                "was not found under ${ESP_VISION_BOARDS_DIR}")
        endif()
    endif()
    set(ESP_VISION_BOARD_MANAGER_PROJECT_DIR "${CMAKE_BINARY_DIR}/esp_vision_board_manager")
    set(ESP_VISION_BOARD_MANAGER_GEN_DIR
        "${ESP_VISION_BOARD_MANAGER_PROJECT_DIR}/components/gen_bmgr_codes")
    file(MAKE_DIRECTORY "${ESP_VISION_BOARD_MANAGER_PROJECT_DIR}")

    execute_process(
        COMMAND
            "${ESP_VISION_PYTHON}"
            "${ESP_VISION_BOARD_MANAGER_DIR}/gen_bmgr_config_codes.py"
            --board "${ESP_VISION_BOARD_MANAGER_BOARD_DIR}"
            --customer-path "${ESP_VISION_BOARD_MANAGER_CUSTOMER_PATH}"
            --project-dir "${ESP_VISION_BOARD_MANAGER_PROJECT_DIR}"
            ${ESP_VISION_BOARD_MANAGER_AMEND_ARGS}
            --skip-sdkconfig-check
            --skip-soc-capability-check
            --log-level WARNING
        RESULT_VARIABLE ESP_VISION_BOARD_MANAGER_RESULT
        OUTPUT_VARIABLE ESP_VISION_BOARD_MANAGER_OUTPUT
        ERROR_VARIABLE ESP_VISION_BOARD_MANAGER_ERROR
    )
    if(NOT ESP_VISION_BOARD_MANAGER_RESULT EQUAL 0)
        message(FATAL_ERROR
            "ESP Board Manager generation failed for ${ESP_VISION_BOARD}:\n"
            "${ESP_VISION_BOARD_MANAGER_OUTPUT}\n"
            "${ESP_VISION_BOARD_MANAGER_ERROR}")
    endif()

    list(APPEND ESP_VISION_BOARD_MANAGER_SOURCES
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}/gen_board_device_config.c"
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}/gen_board_device_handles.c"
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}/gen_board_info.c"
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}/gen_board_periph_config.c"
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}/gen_board_periph_handles.c"
    )
    foreach(generated_source IN LISTS ESP_VISION_BOARD_MANAGER_SOURCES)
        file(READ "${generated_source}" generated_content)
        string(REPLACE "const static " "static const " generated_content "${generated_content}")
        file(WRITE "${generated_source}" "${generated_content}")
    endforeach()
    if(ESP_VISION_BOARD_MANAGER_ESP_BOARDS_SETUP)
        list(APPEND ESP_VISION_BOARD_MANAGER_SOURCES
            "${ESP_VISION_BOARDS_DIR}/${ESP_VISION_BOARD_MANAGER_ESP_BOARDS_SETUP}"
        )
    endif()
    if(ESP_VISION_BOARD_MANAGER_SETUP_SOURCES)
        list(APPEND ESP_VISION_BOARD_MANAGER_SOURCES
            ${ESP_VISION_BOARD_MANAGER_SETUP_SOURCES}
        )
    endif()
    list(APPEND ESP_VISION_BOARD_MANAGER_INCLUDE_DIRS
        "${ESP_VISION_BOARD_MANAGER_GEN_DIR}"
        "${ESP_VISION_BOARD_MANAGER_INPUT_DIR}"
    )

    # Board-manager setup sources are collected into MicroPython's main
    # component. Propagate public headers from board-specific IDF components
    # explicitly so they are available to both IDF 6.x main compile targets.
    foreach(extra_component IN LISTS ESP_VISION_BOARD_MANAGER_EXTRA_COMPONENTS)
        idf_component_get_property(extra_component_lib ${extra_component} COMPONENT_LIB)
        get_target_property(extra_component_include_dirs ${extra_component_lib} INTERFACE_INCLUDE_DIRECTORIES)
        if(extra_component_include_dirs)
            list(APPEND ESP_VISION_BOARD_MANAGER_INCLUDE_DIRS ${extra_component_include_dirs})
        endif()
    endforeach()
endif()

add_library(usermod_esp_vision_platform INTERFACE)

target_sources(usermod_esp_vision_platform INTERFACE
    ${ESP_VISION_MODULE_SOURCES}
    ${ESP_VISION_CAMERA_SOURCE}
    ${ESP_VISION_BOARD_MANAGER_SOURCES}
    ${ESP_VISION_ROOT}/platform/debug.c
    ${ESP_VISION_ROOT}/platform/display.c
    ${ESP_VISION_ROOT}/platform/ev_channel.c
    ${ESP_VISION_ROOT}/platform/ev_control_transport.c
    ${ESP_VISION_ROOT}/platform/ev_mux.c
    ${ESP_VISION_ROOT}/platform/ev_stdio.c
    ${ESP_VISION_ROOT}/platform/jpeg.c
    ${ESP_VISION_ROOT}/platform/preview.c
    ${ESP_VISION_ROOT}/platform/sdcard.c
    ${ESP_VISION_ROOT}/platform/storage.c
    ${ESP_VISION_ROOT}/platform/storage_vfs.c
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
    ${ESP_VISION_BOARD_MANAGER_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}
)

if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
    message(FATAL_ERROR "ESP-IDF PROJECT_VER is required for ESP-VISION firmware identity")
endif()

target_compile_definitions(usermod_esp_vision_platform INTERFACE
    CMSIS_MCU_H="cmsis_compiler.h"
    ESP_VISION_FIRMWARE_VERSION="${PROJECT_VER}"
    OMV_NO_GPL=1
)

if(ESP_VISION_USE_BOARD_MANAGER)
    target_compile_definitions(usermod_esp_vision_platform INTERFACE
        ESP_VISION_USE_BOARD_MANAGER=1
    )
    list(APPEND IDF_COMPONENTS
        espressif__esp_board_manager
        ${ESP_VISION_BOARD_MANAGER_EXTRA_COMPONENTS}
    )
endif()

# MicroPython's qstr preprocessor reads only the main target's direct compile
# definitions, so explicitly provide the version for its user-module pass too.
list(APPEND MICROPY_CPP_DEF_EXTRA
    ESP_VISION_FIRMWARE_VERSION="${PROJECT_VER}"
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

    list(APPEND IDF_COMPONENTS zxing)
endif()

target_compile_options(usermod_esp_vision_platform INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>
)

target_link_libraries(usermod INTERFACE usermod_esp_vision_platform)

include(${CMAKE_CURRENT_LIST_DIR}/lib/ulab/code/micropython.cmake)
