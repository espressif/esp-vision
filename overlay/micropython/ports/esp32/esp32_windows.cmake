# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

# Keep qstr commands below the cmd.exe command-length limit by putting their
# source and compiler arguments in response files.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_target_property(_esp_vision_qstr_inc ${MICROPY_TARGET} INCLUDE_DIRECTORIES)
get_target_property(_esp_vision_qstr_def ${MICROPY_TARGET} COMPILE_DEFINITIONS)
list(APPEND _esp_vision_qstr_inc ${MICROPY_CPP_INC_EXTRA})
list(APPEND _esp_vision_qstr_def ${MICROPY_CPP_DEF_EXTRA})

# These definitions are normally added by mkrules.cmake immediately before it
# collects the target properties.  Put them in the response file now; the
# upstream rules still add the real target definitions when included below.
if(MICROPY_BOARD)
    if(MICROPY_BOARD_VARIANT)
        set(_esp_vision_board_build_name ${MICROPY_BOARD}-${MICROPY_BOARD_VARIANT})
    else()
        set(_esp_vision_board_build_name ${MICROPY_BOARD})
    endif()
    list(APPEND _esp_vision_qstr_def
        "MICROPY_BOARD_BUILD_NAME=\"${_esp_vision_board_build_name}\"")
endif()
if(MICROPY_ROM_TEXT_COMPRESSION)
    list(APPEND _esp_vision_qstr_def "MICROPY_ROM_TEXT_COMPRESSION=(1)")
endif()
if(MICROPY_FROZEN_MANIFEST)
    list(APPEND _esp_vision_qstr_def
        "MICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool"
        "MICROPY_MODULE_FROZEN_MPY=(1)")
endif()
if(MICROPY_PREVIEW_VERSION_2)
    list(APPEND _esp_vision_qstr_def "MICROPY_PREVIEW_VERSION_2=(1)")
endif()

set(_esp_vision_qstr_cpp_flags)
foreach(_arg ${_esp_vision_qstr_inc})
    list(APPEND _esp_vision_qstr_cpp_flags "-I${_arg}")
endforeach()
foreach(_arg ${_esp_vision_qstr_def})
    list(APPEND _esp_vision_qstr_cpp_flags "-D${_arg}")
endforeach()

set(_esp_vision_qstr_flags_rsp "${CMAKE_CURRENT_BINARY_DIR}/micropython-qstr-flags.rsp")
file(WRITE "${_esp_vision_qstr_flags_rsp}" "")
foreach(_arg ${_esp_vision_qstr_cpp_flags})
    string(REPLACE "\"" "\\\"" _rsp_arg "${_arg}")
    if(_rsp_arg MATCHES "[ \t]")
        file(APPEND "${_esp_vision_qstr_flags_rsp}" "\"${_rsp_arg}\"\n")
    else()
        file(APPEND "${_esp_vision_qstr_flags_rsp}" "${_rsp_arg}\n")
    endif()
endforeach()

set(_esp_vision_qstr_sources ${MICROPY_SOURCE_QSTR})
set(_esp_vision_qstr_sources_rsp "${CMAKE_CURRENT_BINARY_DIR}/micropython-qstr-sources.rsp")
file(WRITE "${_esp_vision_qstr_sources_rsp}" "")
foreach(_source ${_esp_vision_qstr_sources})
    file(APPEND "${_esp_vision_qstr_sources_rsp}" "${_source}\n")
endforeach()

set(MICROPY_CPP_FLAGS "@${_esp_vision_qstr_flags_rsp}")
set(MICROPY_CPP_INC)
set(MICROPY_CPP_DEF)
set(MICROPY_CPP_INC_EXTRA)
set(MICROPY_CPP_DEF_EXTRA)
set(MICROPY_SOURCE_QSTR "${_esp_vision_qstr_sources_rsp}")

# MicroPython invokes the Unix utility touch from several generated rules.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/touch.cmd"
    "@echo off\r\n\"${CMAKE_COMMAND}\" -E touch %*\r\n")

# mpy-cross runs on the host.  Build the official Visual Studio project when
# the environment does not already provide a Windows mpy-cross executable.
if("$ENV{MICROPY_MPYCROSS}" STREQUAL "")
    set(_esp_vision_mpy_cross_launcher "${MICROPY_DIR}/tools/build_mpy_cross_windows.py")
    set(_esp_vision_mpy_cross_project "${MICROPY_DIR}/mpy-cross/mpy-cross.vcxproj")
    set(_esp_vision_mpy_cross "${MICROPY_DIR}/mpy-cross/build/mpy-cross.exe")
    add_custom_command(
        OUTPUT "${_esp_vision_mpy_cross}"
        COMMAND "${Python3_EXECUTABLE}" "${_esp_vision_mpy_cross_launcher}"
            "${_esp_vision_mpy_cross_project}"
        DEPENDS
            "${_esp_vision_mpy_cross_launcher}"
            "${_esp_vision_mpy_cross_project}"
        WORKING_DIRECTORY "${MICROPY_DIR}/mpy-cross"
        VERBATIM
    )
    set(MICROPY_MPYCROSS_DEPENDENCY "${_esp_vision_mpy_cross}")
    set(ENV{MICROPY_MPYCROSS} "${_esp_vision_mpy_cross}")
endif()

# Generate a build-local copy of the upstream rules with only the two Unix qstr
# commands replaced.  No complete upstream CMake/Python file is carried here.
set(_esp_vision_qstr_windows "${MICROPY_DIR}/py/qstr_windows.py")
set(_esp_vision_mkrules_windows "${CMAKE_CURRENT_BINARY_DIR}/mkrules-windows.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MICROPY_DIR}/py/mkrules.cmake"
    "${_esp_vision_qstr_windows}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_esp_vision_qstr_windows}" prepare-mkrules
        "${MICROPY_DIR}/py/mkrules.cmake"
        "${_esp_vision_mkrules_windows}"
    RESULT_VARIABLE _esp_vision_prepare_mkrules_result
)
if(NOT _esp_vision_prepare_mkrules_result EQUAL 0)
    message(FATAL_ERROR "Failed to prepare Windows qstr CMake rules")
endif()
include("${_esp_vision_mkrules_windows}")

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_LAST}
    APPEND
    DEPENDS
        ${_esp_vision_qstr_flags_rsp}
        ${_esp_vision_qstr_sources}
        ${_esp_vision_qstr_windows}
        ${MICROPY_PY_DIR}/makeqstrdefs.py
)
add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_PREPROCESSED}
    APPEND
    DEPENDS
        ${_esp_vision_qstr_flags_rsp}
        ${_esp_vision_qstr_windows}
)
