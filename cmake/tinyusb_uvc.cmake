# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

# Build the UVC lifetime changes from a private source copy. ESP-IDF owns and
# verifies the original component download, so it must remain untouched.
function(esp_vision_configure_tinyusb_uvc root)
    idf_component_get_property(tinyusb_target espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(tinyusb_dir espressif__tinyusb COMPONENT_DIR)
    set(patch_file "${root}/tools/patches/tinyusb-uvc-lifetime.patch")
    set(patch_tool "${root}/tools/patch_tinyusb_uvc_barrier.py")
    set(video_transfer "${root}/overlay/micropython/shared/tinyusb/esp_vision_video_transfer.inc")
    set(patched_dir "${CMAKE_BINARY_DIR}/esp_vision_tinyusb")
    set(stamp "${patched_dir}/source.sha256")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${patch_file}" "${patch_tool}" "${video_transfer}" "${tinyusb_dir}/.component_hash")

    file(SHA256 "${patch_file}" patch_hash)
    file(SHA256 "${patch_tool}" tool_hash)
    file(SHA256 "${video_transfer}" transfer_hash)
    file(GLOB_RECURSE component_sources LIST_DIRECTORIES false "${tinyusb_dir}/src/*")
    set(fingerprint "${patch_hash}\n${tool_hash}\n${transfer_hash}\n")
    foreach(source IN LISTS component_sources)
        file(SHA256 "${source}" source_hash)
        string(APPEND fingerprint "${source_hash}\n")
    endforeach()
    string(SHA256 fingerprint "${fingerprint}")
    set(previous "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" previous)
    endif()
    if(NOT previous STREQUAL fingerprint)
        file(REMOVE_RECURSE "${patched_dir}")
        file(MAKE_DIRECTORY "${patched_dir}")
        file(COPY "${tinyusb_dir}/src" "${tinyusb_dir}/LICENSE" DESTINATION "${patched_dir}")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${patch_tool}" "${patched_dir}"
            RESULT_VARIABLE patch_result)
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR "Could not apply the UVC transfer lifetime patch to TinyUSB")
        endif()
        file(COPY "${video_transfer}" DESTINATION "${patched_dir}/src/class/video")
        file(WRITE "${stamp}" "${fingerprint}")
    endif()

    foreach(property SOURCES INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(entries ${tinyusb_target} ${property})
        string(REPLACE "${tinyusb_dir}/src" "${patched_dir}/src" patched "${entries}")
        if(patched STREQUAL entries)
            message(FATAL_ERROR "TinyUSB ${property} does not use the expected component source directory")
        endif()
        set_property(TARGET ${tinyusb_target} PROPERTY ${property} "${patched}")
    endforeach()
    set_source_files_properties("${patched_dir}/src/device/usbd.c"
        TARGET_DIRECTORY ${tinyusb_target} PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
endfunction()
