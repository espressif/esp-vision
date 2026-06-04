# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

set(ESP_VISION_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(ZXING_ROOT "${ESP_VISION_ROOT}/lib/zxing-cpp")
set(ESP_VISION_BOARD "ESP32_P4X_EYE")

if(DEFINED MICROPY_BOARD)
    set(ESP_VISION_BOARD "${MICROPY_BOARD}")
endif()

set(ESP_VISION_ENABLE_BARCODE OFF)
set(ESP_VISION_BOARD_DIR "${ESP_VISION_ROOT}/boards/${ESP_VISION_BOARD}")
if(EXISTS "${ESP_VISION_BOARD_DIR}/board.cmake")
    include("${ESP_VISION_BOARD_DIR}/board.cmake")
endif()


if(NOT EXISTS "${ZXING_ROOT}/core/CMakeLists.txt"
        OR NOT ESP_VISION_ENABLE_BARCODE)
    idf_component_register()
    return()
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES_QT OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_TEST_INSTALL OFF CACHE BOOL "" FORCE)
set(ZXING_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(ZXING_USE_BUNDLED_ZINT OFF CACHE BOOL "" FORCE)
set(ZXING_DISABLE_PCH ON CACHE BOOL "" FORCE)

set(ZXING_ENABLE_1D ON CACHE BOOL "" FORCE)
set(ZXING_ENABLE_AZTEC OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_DATAMATRIX OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_MAXICODE OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_PDF417 OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_QRCODE OFF CACHE BOOL "" FORCE)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
set(CMAKE_SKIP_INSTALL_RULES ON)

idf_component_register(
    SRCS "esp_vision_zxing.cpp"
    INCLUDE_DIRS
        "include"
        "${ESP_VISION_ROOT}/components/imlib/upstream"
        "${ESP_VISION_ROOT}/components/imlib/include"
        "${ESP_VISION_ROOT}/boards/${ESP_VISION_BOARD}"
        "${ESP_VISION_ROOT}/lib/micropython"
        "${ESP_VISION_ROOT}/lib/micropython/ports/esp32"
        "${ESP_VISION_ROOT}/lib/micropython/ports/esp32/boards/${ESP_VISION_BOARD}"
        "${CMAKE_BINARY_DIR}"
)

add_subdirectory("${ZXING_ROOT}/core" "${CMAKE_CURRENT_BINARY_DIR}/zxing-core" EXCLUDE_FROM_ALL)

target_link_libraries(${COMPONENT_LIB} PRIVATE ZXing::ZXing)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fexceptions
    -Wno-error
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CMSIS_MCU_H="cmsis_compiler.h"
)

get_target_property(ZXING_SOURCES ZXing SOURCES)
list(REMOVE_ITEM ZXING_SOURCES
    "src/oned/ODDXFilmEdgeReader.cpp"
    "src/oned/ODDXFilmEdgeReader.h"
    "src/oned/ODReader.cpp"
)
set_target_properties(ZXing PROPERTIES SOURCES "${ZXING_SOURCES}")
set_target_properties(ZXing PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_sources(ZXing PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat/ODReader.cpp")

target_include_directories(ZXing BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/compat"
)

target_compile_options(ZXing PRIVATE
    -fexceptions
    -include "${CMAKE_CURRENT_LIST_DIR}/compat/zxing_config_override.h"
    -Wno-error=return-type
)

if($ENV{IDF_VERSION} VERSION_GREATER_EQUAL "6.0")
    target_compile_options(ZXing PRIVATE -Wno-error=undef)
endif()
