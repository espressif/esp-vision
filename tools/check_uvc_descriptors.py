#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Compile USB class combinations using one existing board build's flags.

This checks descriptor configuration gates without building another firmware.
Use the ESP-IDF Python environment (pyelftools is required for the ELF check).
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(item for item in commands
                 if item['file'].endswith('/shared/tinyusb/mp_usbd_descriptor.c'))
    command = shlex.split(entry['command'])
    # Reuse the real target compiler and flags without producing firmware objects.
    compiler_args = []
    index = 0
    while index < len(command):
        option = command[index]
        if option in ('-o', '-MF', '-MT'):
            index += 2
            continue
        if option not in ('-c', '-MD', '-MMD', entry['file']):
            compiler_args.append(option)
        index += 1
    configurations = {
        'uvc-off': {'MICROPY_HW_USB_UVC': 0},
        'cdc-off': {'MICROPY_HW_USB_CDC': 0},
        'msc-off': {'MICROPY_HW_USB_MSC': 0},
        'video-only': {'MICROPY_HW_USB_CDC': 0, 'MICROPY_HW_USB_MSC': 0},
    }
    for name, overrides in configurations.items():
        source = out / (name + '.c')
        defines = ''.join(f'#undef {key}\n#define {key} {value}\n'
                          for key, value in overrides.items())
        source.write_text('#include "py/mpconfig.h"\n' + defines
                          + f'#include "{entry["file"]}"\n')
        result = subprocess.run(compiler_args + ['-fsyntax-only', str(source)],
                                cwd=entry['directory'], capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.stderr)
        print('PASS descriptor compile', name)

    from elftools.elf.elffile import ELFFile
    with (build / 'esp-vision.elf').open('rb') as binary:
        elf = ELFFile(binary)
        symbol = elf.get_section_by_name('.symtab').get_symbol_by_name('mp_usbd_builtin_desc_cfg')[0]
        section = elf.get_section(symbol['st_shndx'])
        offset = symbol['st_value'] - section['sh_addr']
        data = section.data()[offset:offset + symbol['st_size']]
    assert int.from_bytes(data[2:4], 'little') == len(data)
    interfaces, endpoints = set(), set()
    pos = 0
    while pos < len(data):
        size = data[pos]
        assert size >= 2 and pos + size <= len(data), (pos, size)
        descriptor = data[pos:pos + size]
        if descriptor[1] == 4:
            interfaces.add(descriptor[2])
        if descriptor[1] == 5:
            assert descriptor[2] not in endpoints, descriptor[2]
            endpoints.add(descriptor[2])
        pos += size
    assert interfaces == set(range(data[4])), interfaces
    print('PASS final ELF descriptor lengths, interfaces and unique endpoints')


if __name__ == '__main__':
    main()
