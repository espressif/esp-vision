#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Check UVC mode tables and, optionally, an existing firmware build.

Running this script without arguments performs source-only checks. It does not
build firmware or connect to hardware. Passing --build-dir additionally checks
an existing build's descriptor object and compile-time USB class gates.
"""

import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess


USB_OTG_TARGETS = {"esp32p4", "esp32s2", "esp32s3", "esp32s31"}
# UVC discrete intervals are ordered by numeric value for TinyUSB's MIN/MAX
# handling. The frame descriptor's default interval remains the final (10 fps)
# value so the released mode stays the initial negotiated mode.
FPS_INTERVALS = (333333, 500000, 1000000)
FRAME_CAP = 49152
TARGET_MODE_PROFILES = {
    "AtomS3R-M12": [((320, 240), FPS_INTERVALS)],
    "ESP32_P4X_EYE": [((320, 240), FPS_INTERVALS), ((240, 240), FPS_INTERVALS),
                       ((1280, 720), FPS_INTERVALS)],
    "ESP32_P4X_VISION": [((320, 240), FPS_INTERVALS), ((1280, 720), FPS_INTERVALS)],
    "ESP32_P4X_FUNCTION_EV_BOARD": [((320, 240), FPS_INTERVALS), ((1024, 600), FPS_INTERVALS),
                                    ((1280, 720), FPS_INTERVALS)],
    "ESP32_S3_EYE": [((320, 240), FPS_INTERVALS), ((240, 240), FPS_INTERVALS)],
    "ESP32_S31_MOSAICO": [((320, 240), (1000000,)),
                          ((480, 480), FPS_INTERVALS),
                          ((640, 480), FPS_INTERVALS)],
}
EXPECTED_DIMENSIONS = {name: [profile[0] for profile in profiles]
                       for name, profiles in TARGET_MODE_PROFILES.items()}
UNVERIFIED_NO_PROFILE_BOARDS = {"ESP32_S31_KORVO"}
FALLBACK = (1, 320, 240, 49152, 1, 1000000, 0, 0)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def macro_body(text, name, function_arg=None):
    """Return a #define replacement body, joining backslash continuations."""
    signature = re.escape(name)
    if function_arg is not None:
        signature += r"\(" + re.escape(function_arg) + r"\)"
    pattern = re.compile(r"^\s*#\s*define\s+" + signature + r"\s*(.*)$", re.M)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = pattern.match(line)
        if not match:
            continue
        parts = []
        current = match.group(1).rstrip()
        while True:
            continuation = current.endswith("\\")
            parts.append(current[:-1].strip() if continuation else current.strip())
            if not continuation:
                break
            index += 1
            require(index < len(lines), f"unterminated macro {name}")
            current = lines[index].rstrip()
        return " ".join(part for part in parts if part)
    return None


def parse_x_rows(body, macro_name):
    require(body is not None, f"missing {macro_name}")
    rows = []
    for match in re.finditer(r"\bX\(([^()]*)\)", body):
        fields = [field.strip() for field in match.group(1).split(",")]
        try:
            row = tuple(int(field, 0) for field in fields)
        except ValueError as error:
            raise AssertionError(f"invalid integer in {macro_name}: {fields}") from error
        require(len(row) == 8, f"{macro_name} rows must have 8 fields: {row}")
        rows.append(row)
    require(rows, f"{macro_name} contains no modes")
    return rows


def int_define(text, name):
    match = re.search(r"^\s*#\s*define\s+" + re.escape(name) + r"\s+\(?\s*(0[xX][0-9a-fA-F]+|\d+)\s*\)?", text, re.M)
    return int(match.group(1), 0) if match else None


def configured_dimensions(boardconfig):
    dimensions = set()
    prefixes = [
        "ESP_VISION_CAMERA_RAW_INPUT",
        "ESP_VISION_CAMERA_ACTIVE_INPUT",
        "ESP_VISION_CAMERA_OUTPUT_QQVGA",
        "ESP_VISION_CAMERA_OUTPUT_QVGA",
        "ESP_VISION_CAMERA_OUTPUT_VGA",
        "ESP_VISION_CAMERA_PPA_OUTPUT_QQVGA",
        "ESP_VISION_CAMERA_PPA_OUTPUT_QVGA",
        "ESP_VISION_CAMERA_PPA_OUTPUT_VGA",
        "ESP_VISION_LCD",
    ]
    for prefix in prefixes:
        width = int_define(boardconfig, prefix + "_WIDTH")
        height = int_define(boardconfig, prefix + "_HEIGHT")
        if width is not None and height is not None:
            dimensions.add((width, height))
    return dimensions


def validate_rows(rows, label, target):
    indices = [row[0] for row in rows]
    dimensions = [(row[1], row[2]) for row in rows]
    require(indices == list(range(1, len(rows) + 1)), f"{label}: frame indexes must be unique and contiguous: {indices}")
    require(len(set(dimensions)) == len(dimensions), f"{label}: duplicate frame dimensions: {dimensions}")
    for index, width, height, max_size, count, first, second, third in rows:
        intervals = (first, second, third)[:count]
        require(1 <= count <= 3, f"{label}: frame {index} interval count must be 1..3")
        require(width > 0 and height > 0 and max_size > 0, f"{label}: invalid dimensions or frame size")
        if target:
            require(max_size == FRAME_CAP,
                    f"{label}: every target frame must retain the {FRAME_CAP}-byte cap")
        require(all(interval > 0 for interval in intervals), f"{label}: zero frame interval")
        require(list(intervals) == sorted(set(intervals)),
                f"{label}: intervals must be unique and ascending: {intervals}")
    if target:
        profiles = TARGET_MODE_PROFILES.get(label)
        require(profiles is not None, f"{label}: no target profile is defined")
        expected = [(dimension, intervals) for dimension, intervals in profiles]
        actual = [((row[1], row[2]), row[5:5 + row[4]]) for row in rows]
        require(actual == expected, f"{label}: expected profile {expected}, got {actual}")
        require(rows[0][1:3] == (320, 240) and
                rows[0][5 + rows[0][4] - 1] == 1000000,
                f"{label}: target table must start with released 320x240@10")


def frame_bytes(row):
    index, width, height, max_size, count, first, second, third = row
    intervals = (first, second, third)[:count]
    slowest = max(intervals)
    fastest = min(intervals)
    min_rate = 8192 * 10_000_000 // slowest
    max_rate = max_size * 8 * 10_000_000 // fastest
    length = 26 + 4 * count
    head = struct.pack("<BBBBBHHIIIIB", length, 0x24, 0x07, index, 0,
                       width, height, min_rate, max_rate, max_size, intervals[-1], count)
    return head + struct.pack("<" + "I" * count, *intervals)


def validate_serialized_modes(rows, label, fixed_bytes):
    descriptors = [frame_bytes(row) for row in rows]
    require(all(len(desc) == desc[0] == 26 + 4 * desc[25] for desc in descriptors),
            f"{label}: frame descriptor length mismatch")
    for row, desc in zip(rows, descriptors):
        index, width, height = row[:3]
        count = row[4]
        require(desc[3] == index, f"{label}: serialized frame index mismatch")
        require(int.from_bytes(desc[5:7], "little") == width, f"{label}: serialized width mismatch")
        require(int.from_bytes(desc[7:9], "little") == height, f"{label}: serialized height mismatch")
        require(desc[25] == count, f"{label}: serialized interval count mismatch")
        intervals = struct.unpack_from("<" + "I" * count, desc, 26)
        require(intervals == row[5:5 + count], f"{label}: serialized intervals mismatch")
        require(int.from_bytes(desc[21:25], "little") == intervals[-1],
                f"{label}: default interval mismatch")
        require(int.from_bytes(desc[9:13], "little") == 8192 * 10_000_000 // max(intervals),
                f"{label}: min bitrate does not match the slowest interval")
        require(int.from_bytes(desc[13:17], "little") == row[3] * 8 * 10_000_000 // min(intervals),
                f"{label}: max bitrate does not match the fastest interval")
    class_specific_length = fixed_bytes + sum(len(desc) for desc in descriptors)
    return descriptors, class_specific_length


def find_usbdev_boards(root):
    boards = []
    for board_dir in sorted((root / "boards").iterdir()):
        header = board_dir / "port" / "mpconfigboard.h"
        cmake = board_dir / "port" / "mpconfigboard.cmake"
        if not header.is_file() or not cmake.is_file():
            continue
        header_text = header.read_text()
        explicit = int_define(header_text, "MICROPY_HW_ENABLE_USBDEV")
        target_match = re.search(r"set\s*\(\s*IDF_TARGET\s+(\w+)", cmake.read_text())
        target = target_match.group(1) if target_match else ""
        enabled = bool(explicit) if explicit is not None else target in USB_OTG_TARGETS
        if enabled:
            boards.append((board_dir.name, header_text))
    return boards


def test_capability_gate(descriptor_source):
    start = descriptor_source.index("#ifndef MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES")
    end = descriptor_source.index("#if MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES && defined", start)
    gate = descriptor_source[start:end]
    require("MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE" in gate and "#error" in gate,
            "target mode capability gate is missing")
    require("#if MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES && !MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE" in gate,
            "target mode capability condition changed")
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        print("SKIP C preprocessor capability gate (no host C compiler)")
        return
    for enabled, capable, succeeds in ((0, 0, True), (1, 0, False), (1, 1, True)):
        source = (f"#define MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES {enabled}\n"
                  f"#define MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE {capable}\n"
                  f"{gate}\nint gate_case;\n")
        result = subprocess.run([compiler, "-E", "-x", "c", "-"], input=source,
                                capture_output=True, text=True)
        require((result.returncode == 0) == succeeds,
                f"capability gate failed for enable={enabled}, capable={capable}: {result.stderr}")
    print("PASS target mode capability gate")


def test_unverified_board_gate(root):
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        print("SKIP unverified board target-mode gate (no host C compiler)")
        return
    header = root / "boards/ESP32_S31_KORVO/port/mpconfigboard.h"
    source = f'#include "{header}"\nint gate_case;\n'
    for enabled, capable, succeeds in ((0, 0, True), (1, 1, False)):
        case_source = (f"#define MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES {enabled}\n"
                       f"#define MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE {capable}\n"
                       f"{source}")
        result = subprocess.run([compiler, "-E", "-x", "c", "-"], input=case_source,
                                capture_output=True, text=True, cwd=root)
        require((result.returncode == 0) == succeeds,
                f"Korvo target-mode gate failed for enable={enabled}, capable={capable}: {result.stderr}")
        if not succeeds:
            require("ESP32_S31_KORVO has no verified UVC target mode profile" in result.stderr,
                    "Korvo target-mode rejection did not come from its missing-profile guard")
    print("PASS Korvo unverified target-mode gate")


def c_function_body(source, name):
    match = re.search(r"(?:static\s+)?(?:void|mp_obj_t)\s+" + re.escape(name) +
                      r"\s*\([^;]*\)\s*\{", source)
    require(match is not None, f"missing C function {name}")
    opening = source.find("{", match.start())
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():position + 1]
    raise AssertionError(f"unterminated C function {name}")


def test_mode_callback_wiring(root):
    source = (root / "modules/py_uvc.c").read_text()
    require("MP_REGISTER_ROOT_POINTER(mp_obj_t py_uvc_mode_callback)" in source,
            "UVC Python callback is not protected by a MicroPython root pointer")
    commit = c_function_body(source, "py_uvc_mode_committed")
    require("mp_sched_schedule_node(&py_uvc_mode_sched_node" in commit and
            "mp_hal_wake_main_task()" in commit,
            "Commit hook must defer callback work to the VM scheduler")
    require("mp_call_function" not in commit and "mp_obj_new" not in commit,
            "Commit hook must not enter Python or allocate MicroPython objects")
    dispatch = c_function_body(source, "py_uvc_mode_callback_dispatch")
    require("nlr_push(&nlr)" in dispatch and "mp_obj_print_exception" in dispatch,
            "mode dictionary allocation and callback invocation must catch exceptions")
    setter = c_function_body(source, "py_uvc_set_mode_callback")
    require("py_uvc_mode_node_remove" not in setter,
            "normal callback registration must not unlink a scheduler node")
    reset = c_function_body(source, "py_uvc_soft_reset")
    require(reset.index("esp_vision_uvc_set_mode_committed_cb(NULL)") <
            reset.index("py_uvc_mode_node_remove()"),
            "soft reset must disable the Commit producer before removing its node")
    require("MP_STATE_PORT(py_uvc_mode_callback) = mp_const_none" in reset,
            "soft reset must clear the callback root before heap collection")
    print("PASS VM callback scheduling and soft-reset lifecycle")


def source_check(root):
    descriptor_path = root / "overlay/micropython/shared/tinyusb/mp_usbd_descriptor.c"
    tinyusb_video = root / "lib/micropython/lib/tinyusb/src/class/video/video.h"
    descriptor_source = descriptor_path.read_text()
    public_header = (root / "overlay/micropython/shared/tinyusb/mp_usbd.h").read_text()
    video_source = tinyusb_video.read_text()
    require("MP_USBD_UVC_FRAME_DESC_HEADER" in descriptor_source and
            "VIDEO_CS_ITF_VS_FRAME_MJPEG" in descriptor_source,
            "descriptor does not emit MJPEG discrete-frame headers")
    require("TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC(" not in descriptor_source,
            "TinyUSB variadic frame macro must not receive 32-bit intervals as raw bytes")
    for interval_name in ("interval0", "interval1", "interval2"):
        require(f"U32_TO_U8S_LE({interval_name})" in descriptor_source,
                f"{interval_name} is not serialized as a little-endian 32-bit value")
    require("interval2" in descriptor_source and "interval1" in descriptor_source,
            "descriptor bitrate calculations do not account for interval count")
    require("mp_usbd_uvc_default_mode" in public_header and
            "mp_usbd_uvc_mode_get" in public_header,
            "runtime lookup of advertised modes is missing")
    require("#define MP_USBD_BUILTIN_DESC_CFG_LEN" in public_header and
            "#define MP_USBD_BUILTIN_DESC_CFG_LEN" not in descriptor_source,
            "built-in configuration length must have one shared header definition")
    machine_source = (root / "lib/micropython/extmod/machine_usb_device.c").read_text()
    require("mp_usbd_builtin_desc_cfg, MP_USBD_BUILTIN_DESC_CFG_LEN" in machine_source,
            "MicroPython built-in USB descriptor consumer lost the upstream length interface")
    fallback = parse_x_rows(macro_body(public_header, "MP_USBD_UVC_FALLBACK_MODE_LIST", "X"),
                            "MP_USBD_UVC_FALLBACK_MODE_LIST")
    require(fallback == [FALLBACK], f"default fallback changed: {fallback}")
    validate_rows(fallback, "fallback", target=False)

    lengths = {}
    for name in ("TUD_VIDEO_DESC_IAD_LEN", "TUD_VIDEO_DESC_STD_VC_LEN", "TUD_VIDEO_DESC_CS_VC_LEN",
                 "TUD_VIDEO_DESC_CAMERA_TERM_LEN", "TUD_VIDEO_DESC_OUTPUT_TERM_LEN",
                 "TUD_VIDEO_DESC_STD_VS_LEN", "TUD_VIDEO_DESC_CS_VS_IN_LEN",
                 "TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN", "TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN"):
        match = re.search(r"^\s*#\s*define\s+" + name + r"\s+(\d+)\s*$", video_source, re.M)
        require(match is not None, f"cannot find TinyUSB length {name}")
        lengths[name] = int(match.group(1))
    fixed = (lengths["TUD_VIDEO_DESC_IAD_LEN"] + lengths["TUD_VIDEO_DESC_STD_VC_LEN"] +
             lengths["TUD_VIDEO_DESC_CS_VC_LEN"] + 1 + lengths["TUD_VIDEO_DESC_CAMERA_TERM_LEN"] +
             lengths["TUD_VIDEO_DESC_OUTPUT_TERM_LEN"] + lengths["TUD_VIDEO_DESC_STD_VS_LEN"] +
             lengths["TUD_VIDEO_DESC_CS_VS_IN_LEN"] + 1 + lengths["TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN"] +
             lengths["TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN"] + lengths["TUD_VIDEO_DESC_STD_VS_LEN"] + 7)
    require(fixed == 113, f"unexpected fixed UVC descriptor length: {fixed}")
    fallback_descs, fallback_length = validate_serialized_modes(fallback, "fallback", fixed)
    require(len(fallback_descs[0]) == 30 and fallback_length == 143,
            f"default 320x240@10 descriptor length mismatch: {fallback_length}")

    usbdev_boards = find_usbdev_boards(root)
    names = {name for name, _ in usbdev_boards}
    checked_boards = set(EXPECTED_DIMENSIONS) | UNVERIFIED_NO_PROFILE_BOARDS
    require(checked_boards.issubset(names),
            f"missing USB Device board mode checks: {sorted(checked_boards - names)}")
    checked = 0
    for board_name, header in usbdev_boards:
        enable = int_define(header, "MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES")
        body = macro_body(header, "MICROPY_HW_USB_UVC_TARGET_MODE_LIST", "X")
        if board_name == "TEMPLATE":
            require(enable in (None, 0), "template board must not enable target modes")
            require(body is None, "template board must not claim camera target modes")
            continue
        require(enable == 0, f"{board_name}: unverified target modes must stay disabled by default")
        if board_name in UNVERIFIED_NO_PROFILE_BOARDS:
            require(body is None,
                    f"{board_name}: do not publish a mode table while sensor variants select different raw inputs")
            continue
        require(board_name in EXPECTED_DIMENSIONS, f"{board_name}: missing expected mode profile")
        rows = parse_x_rows(body, f"{board_name} MICROPY_HW_USB_UVC_TARGET_MODE_LIST")
        validate_rows(rows, board_name, target=True)
        boardconfig_path = root / "boards" / board_name / "boardconfig.h"
        physical_dimensions = configured_dimensions(boardconfig_path.read_text())
        target_dimensions = {(row[1], row[2]) for row in rows}
        candidate_dimensions = target_dimensions - {(320, 240)}
        require(candidate_dimensions.issubset(physical_dimensions),
                f"{board_name}: candidate size lacks a matching board camera/screen configuration")
        descriptors, uvc_length = validate_serialized_modes(rows, board_name, fixed)
        frame_bytes_total = sum(len(desc) for desc in descriptors)
        expected_vs_length = (lengths["TUD_VIDEO_DESC_CS_VS_IN_LEN"] + 1 +
                              lengths["TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN"] + frame_bytes_total +
                              lengths["TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN"])
        require(expected_vs_length == 31 + frame_bytes_total,
                f"{board_name}: class-specific VS total length mismatch")
        require(uvc_length == fixed + frame_bytes_total,
                f"{board_name}: UVC block total length mismatch")
        checked += 1
    require(checked == len(EXPECTED_DIMENSIONS), f"checked {checked} candidate board profiles")
    test_capability_gate(public_header)
    test_unverified_board_gate(root)
    test_mode_callback_wiring(root)
    print(f"PASS source mode tables, fallback descriptor length, discrete intervals, and indexes ({checked} boards)")


def parse_descriptors(data):
    descriptors = []
    position = 0
    while position < len(data):
        length = data[position]
        require(length >= 2 and position + length <= len(data),
                f"invalid descriptor at offset {position}, length {length}")
        descriptors.append(data[position:position + length])
        position += length
    require(position == len(data), "configuration descriptor walk did not end cleanly")
    return descriptors


def validate_elf_descriptor(data):
    require(len(data) >= 9 and int.from_bytes(data[2:4], "little") == len(data),
            "configuration descriptor wTotalLength mismatch")
    descriptors = parse_descriptors(data)
    interfaces = {desc[2] for desc in descriptors if desc[1] == 4}
    endpoints = [desc[2] for desc in descriptors if desc[1] == 5]
    require(len(endpoints) == len(set(endpoints)), "duplicate endpoint address")
    require(interfaces == set(range(data[4])), f"interface list mismatch: {interfaces}")

    formats = [desc for desc in descriptors if desc[1] == 0x24 and len(desc) >= 11 and desc[2] == 0x06]
    frames = [desc for desc in descriptors if desc[1] == 0x24 and len(desc) >= 26 and desc[2] == 0x07]
    require(len(formats) == 1, f"expected one MJPEG format descriptor, found {len(formats)}")
    require(formats[0][4] == len(frames), "MJPEG frame count does not match format descriptor")
    require(formats[0][6] == 1, "MJPEG format must keep frame 1 as the default size")
    camera_terminals = [desc for desc in descriptors
                        if desc[1] == 0x24 and len(desc) >= 18 and desc[2] == 0x02]
    require(len(camera_terminals) == 1, f"expected one camera input terminal, found {len(camera_terminals)}")
    require(camera_terminals[0][14] == 3 and camera_terminals[0][15:18] == b"\0\0\0",
            "UVC camera terminal must expose no reverse camera controls")
    indices, dimensions = [], []
    for desc in frames:
        count = desc[25]
        require(len(desc) == desc[0] == 26 + 4 * count, "MJPEG frame descriptor length mismatch")
        require(count > 0, "MJPEG frame has no discrete interval")
        intervals = struct.unpack_from("<" + "I" * count, desc, 26)
        require(tuple(intervals) == tuple(sorted(set(intervals))),
                "MJPEG intervals must be unique and ascending")
        require(int.from_bytes(desc[21:25], "little") == intervals[-1],
                "default interval is not the slowest supported rate")
        indices.append(desc[3])
        dimensions.append((int.from_bytes(desc[5:7], "little"), int.from_bytes(desc[7:9], "little")))
        require(int.from_bytes(desc[9:13], "little") == 8192 * 10_000_000 // max(intervals),
                "MJPEG minimum bitrate mismatch")
        require(int.from_bytes(desc[13:17], "little") == int.from_bytes(desc[17:21], "little") * 8 * 10_000_000 // min(intervals),
                "MJPEG maximum bitrate mismatch")
    require(indices == list(range(1, len(frames) + 1)), f"frame indexes are not contiguous: {indices}")
    require(len(dimensions) == len(set(dimensions)), f"duplicate MJPEG frame dimensions: {dimensions}")
    vs_headers = [desc for desc in descriptors if desc[1] == 0x24 and len(desc) >= 14 and desc[2] == 0x01]
    if vs_headers:
        header = vs_headers[-1]
        class_specific = 0
        for desc in descriptors[descriptors.index(header):]:
            if desc is not header and desc[1] == 4:
                break
            if desc[1] == 0x24:
                class_specific += desc[0]
        require(int.from_bytes(header[4:6], "little") == class_specific,
                "UVC VS header wTotalLength mismatch")
    print(f"PASS ELF UVC descriptors ({len(frames)} frames, {dimensions})")
    return frames


def extract_elf_symbol(path, symbol_name):
    data = path.read_bytes()
    require(data[:4] == b"\x7fELF", f"not an ELF file: {path}")
    elf_class, byte_order = data[4], data[5]
    require(byte_order == 1, f"unsupported big-endian ELF: {path}")
    if elf_class == 1:
        header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        section_fmt, symbol_fmt = "<IIIIIIIIII", "<IIIBBH"
    elif elf_class == 2:
        header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        section_fmt, symbol_fmt = "<IIQQQQIIQQ", "<IBBHQQ"
    else:
        raise AssertionError(f"unsupported ELF class {elf_class}: {path}")
    section_offset, section_size, section_count = header[5], header[10], header[11]
    require(section_count > 0 and section_size >= struct.calcsize(section_fmt),
            f"invalid ELF section table: {path}")
    sections = []
    for index in range(section_count):
        values = struct.unpack_from(section_fmt, data, section_offset + index * section_size)
        if elf_class == 1:
            name, section_type, address, offset, size, link, entry_size = (
                values[0], values[1], values[3], values[4], values[5], values[6], values[9])
        else:
            name, section_type, address, offset, size, link, entry_size = (
                values[0], values[1], values[3], values[4], values[5], values[6], values[9])
        sections.append({"index": index, "name": name, "type": section_type,
                         "address": address, "offset": offset, "size": size,
                         "link": link, "entry_size": entry_size})

    for section in sections:
        if section["type"] != 2 or section["entry_size"] < struct.calcsize(symbol_fmt):
            continue
        require(section["link"] < len(sections), f"invalid ELF string table link: {path}")
        strings_section = sections[section["link"]]
        strings = data[strings_section["offset"]:strings_section["offset"] + strings_section["size"]]
        symbol_count = section["size"] // section["entry_size"]
        for index in range(symbol_count):
            values = struct.unpack_from(symbol_fmt, data,
                                        section["offset"] + index * section["entry_size"])
            if elf_class == 1:
                name_offset, value, size, _, _, section_index = values
            else:
                name_offset, _, _, section_index, value, size = values
            if name_offset >= len(strings):
                continue
            name_end = strings.find(b"\0", name_offset)
            name = strings[name_offset:name_end].decode("utf-8", errors="replace")
            if name != symbol_name:
                continue
            require(section_index < len(sections), f"{symbol_name} has no data section in {path}")
            symbol_section = sections[section_index]
            symbol_offset = symbol_section["offset"] + value - symbol_section["address"]
            require(symbol_offset >= symbol_section["offset"] and
                    symbol_offset + size <= symbol_section["offset"] + symbol_section["size"],
                    f"{symbol_name} is outside its ELF section in {path}")
            return data[symbol_offset:symbol_offset + size]
    raise AssertionError(f"{symbol_name} symbol not found in {path}")


def expand_response_files(command, cwd):
    expanded = []
    for arg in command:
        if not arg.startswith("@"):
            expanded.append(arg)
            continue
        response = Path(arg[1:])
        if not response.is_absolute():
            response = cwd / response
        if response.is_file():
            expanded.extend(shlex.split(response.read_text()))
        else:
            expanded.append(arg)
    return expanded


def command_define_int(command, name):
    prefix = "-D" + name
    value = None
    for index, arg in enumerate(command):
        if arg == "-D" and index + 1 < len(command):
            definition = command[index + 1]
            if definition == name:
                value = "1"
            elif definition.startswith(name + "="):
                value = definition.split("=", 1)[1]
        elif arg == prefix:
            value = "1"
        elif arg.startswith(prefix + "="):
            value = arg.split("=", 1)[1]
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError as error:
        raise AssertionError(f"non-integer -D{ name } value: {value}") from error


def build_check(build_dir, output_dir, root, descriptor_only=False):
    build = build_dir.resolve()
    out = output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in commands if item["file"].endswith("/shared/tinyusb/mp_usbd_descriptor.c"))
    command = shlex.split(entry["command"]) if "command" in entry else entry["arguments"]
    expanded_command = expand_response_files(command, Path(entry["directory"]))
    board_match = next((arg for arg in command
                        if arg.startswith("-DMICROPY_BOARD_BUILD_NAME=")), None)
    require(board_match is not None, "cannot infer board name from descriptor compile command")
    board_name = board_match.split("=", 1)[1].strip("\"'")
    board_header = (root / "boards" / board_name / "port" / "mpconfigboard.h").read_text()
    target_body = macro_body(board_header, "MICROPY_HW_USB_UVC_TARGET_MODE_LIST", "X")
    expected_target_rows = (parse_x_rows(target_body, f"{board_name} target mode list")
                            if target_body is not None else [FALLBACK])
    descriptor_source = root / "overlay/micropython/shared/tinyusb/mp_usbd_descriptor.c"
    compiler_args = []
    index = 0
    while index < len(command):
        option = command[index]
        if option in ("-o", "-MF", "-MT"):
            index += 2
            continue
        if option not in ("-c", "-MD", "-MMD", entry["file"]):
            compiler_args.append(option)
        index += 1
    configurations = {
        "uvc-off": ({"MICROPY_HW_USB_UVC": 0}, True),
        "cdc-off": ({"MICROPY_HW_USB_CDC": 0}, True),
        "msc-off": ({"MICROPY_HW_USB_MSC": 0}, True),
        "video-only": ({"MICROPY_HW_USB_CDC": 0, "MICROPY_HW_USB_MSC": 0}, True),
        "target-capability-rejected": ({"MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES": 1,
                                        "MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE": 0}, False),
        "target-capability-enabled": ({"MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES": 1,
                                       "MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE": 1}, True),
    }
    for name, (overrides, expect_success) in configurations.items():
        source = out / (name + ".c")
        object_file = out / (name + ".o")
        defines = "".join(f"#undef {key}\n#define {key} {value}\n" for key, value in overrides.items())
        mode_list = ""
        if name == "target-capability-enabled" and target_body is not None:
            row_calls = " ".join("X(" + ",".join(str(field) for field in row) + ")"
                                  for row in expected_target_rows)
            mode_list = ("#undef MICROPY_HW_USB_UVC_TARGET_MODE_LIST\n"
                         f"#define MICROPY_HW_USB_UVC_TARGET_MODE_LIST(X) {row_calls}\n")
        source.write_text('#include "py/mpconfig.h"\n' + defines + mode_list +
                          f'#include "{descriptor_source}"\n')
        compile_target_object = name == "target-capability-enabled"
        compile_args = compiler_args + (["-c", str(source), "-o", str(object_file)]
                                        if compile_target_object else ["-fsyntax-only", str(source)])
        result = subprocess.run(compile_args,
                                cwd=entry["directory"], capture_output=True, text=True)
        (out / (name + ".log")).write_text(result.stdout + result.stderr)
        require((result.returncode == 0) == expect_success,
                f"descriptor compile case {name} unexpected result: {result.stderr}")
        if not expect_success:
            require("UVC target modes require" in result.stderr,
                    "target mode rejection did not come from the capability gate")
        print("PASS descriptor compile", name)

    target_data = extract_elf_symbol(object_file, "mp_usbd_builtin_desc_cfg")
    target_frames = validate_elf_descriptor(target_data)
    require(len(target_frames) == len(expected_target_rows),
            f"compiled target table has {len(target_frames)} frames; expected {len(expected_target_rows)}")
    for row, frame in zip(expected_target_rows, target_frames):
        index, width, height, max_size, count, first, second, third = row
        intervals = (first, second, third)[:count]
        require(frame[3] == index and
                int.from_bytes(frame[5:7], "little") == width and
                int.from_bytes(frame[7:9], "little") == height and
                int.from_bytes(frame[17:21], "little") == max_size and
                frame[25] == count,
                f"compiled target descriptor does not match frame {index} in {board_name}")
        require(struct.unpack_from("<" + "I" * count, frame, 26) == intervals,
                f"compiled target interval bytes do not match frame {index} in {board_name}")
    print(f"PASS compiled target descriptor bytes ({board_name}, {len(target_frames)} frames)")

    if descriptor_only:
        print("SKIP existing firmware ELF check (descriptor-only run)")
        return

    data = extract_elf_symbol(build / "esp-vision.elf", "mp_usbd_builtin_desc_cfg")
    validate_elf_descriptor(data)
    frame_descs = [desc for desc in parse_descriptors(data) if desc[1] == 0x24 and len(desc) >= 26 and desc[2] == 0x07]
    target_modes_enabled = command_define_int(
        expanded_command, "MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES")
    if target_modes_enabled is None:
        target_modes_enabled = int_define(board_header, "MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES") or 0
    mode_table_capable = command_define_int(
        expanded_command, "MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE") or 0
    if target_modes_enabled:
        require(mode_table_capable == 1,
                "firmware enables target UVC modes without mode-table capability")
        expected_firmware_rows = expected_target_rows
        firmware_label = "experimental target table"
    else:
        expected_firmware_rows = [FALLBACK]
        firmware_label = "fallback table"
    require(len(frame_descs) == len(expected_firmware_rows),
            f"ELF {firmware_label} has {len(frame_descs)} frames; "
            f"expected {len(expected_firmware_rows)}")
    for row, frame in zip(expected_firmware_rows, frame_descs):
        index, width, height, max_size, count, first, second, third = row
        intervals = (first, second, third)[:count]
        require(frame[3] == index and
                int.from_bytes(frame[5:7], "little") == width and
                int.from_bytes(frame[7:9], "little") == height and
                int.from_bytes(frame[17:21], "little") == max_size and
                frame[25] == count,
                f"ELF {firmware_label} does not match frame {index} in {board_name}")
        require(struct.unpack_from("<" + "I" * count, frame, 26) == intervals,
                f"ELF {firmware_label} interval bytes do not match frame {index} in {board_name}")
    print(f"PASS ELF {firmware_label} ({board_name}, {len(frame_descs)} frames)")


def main():
    root_default = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=root_default)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--descriptor-only", action="store_true",
                        help="compile current target descriptors with the existing build invocation; skip its possibly stale firmware ELF")
    args = parser.parse_args()
    require((args.build_dir is None) == (args.output_dir is None),
            "--build-dir and --output-dir must be provided together")
    require(not args.descriptor_only or args.build_dir is not None,
            "--descriptor-only requires --build-dir and --output-dir")
    source_check(args.source_root.resolve())
    if args.build_dir is not None:
        build_check(args.build_dir, args.output_dir, args.source_root.resolve(), args.descriptor_only)


if __name__ == "__main__":
    main()
