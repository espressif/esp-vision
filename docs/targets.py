# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Target and board capabilities used by the documentation toolchain.

Keep target-dependent API selection tied to the firmware build configuration.
The parsers below intentionally cover only the small CMake patterns used by
ESP-VISION; an unsupported pattern fails the documentation build instead of
silently publishing stale API availability.
"""

import re
from pathlib import Path

DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent
MICROPYTHON_CMAKE = REPO_ROOT / 'micropython.cmake'
MP_CONFIG_PORT = REPO_ROOT / 'overlay/micropython/ports/esp32/mpconfigport.h'
BOARD_ROOT = REPO_ROOT / 'boards'
# Each board's MicroPython-port files live under boards/<BOARD>/port/ and are
# projected onto ports/esp32/boards/<BOARD>/ at build time.
MP_BOARD_ROOT = BOARD_ROOT

TARGETS = ('esp32s31', 'esp32p4', 'esp32s3')
DOCUMENTED_MODULES = ('sensor', 'image', 'display', 'espdl', 'tflite', 'imageio', 'h264', 'rtsp')

_SOURCE_MODULES = {
    'py_sensor': 'sensor',
    'py_image': 'image',
    'py_display': 'display',
    'py_espdl': 'espdl',
    'py_tflite': 'tflite',
    'py_imageio': 'imageio',
    'py_h264': 'h264',
    'py_rtsp': 'rtsp',
}


def _modules_from_source_block(block):
    names = re.findall(r'/([A-Za-z0-9_]+)\.(?:c|cpp)\b', block)
    return {_SOURCE_MODULES[name] for name in names if name in _SOURCE_MODULES}


def _read_module_targets():
    text = MICROPYTHON_CMAKE.read_text(encoding='utf-8')
    common_match = re.search(
        r'set\(ESP_VISION_MODULE_SOURCES(?P<body>.*?)^\)',
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if common_match is None:
        raise RuntimeError(
            'Cannot find ESP_VISION_MODULE_SOURCES in {}'.format(MICROPYTHON_CMAKE)
        )

    modules = {
        target: set(_modules_from_source_block(common_match.group('body')))
        for target in TARGETS
    }
    conditional_blocks = re.finditer(
        r'if\((?P<condition>.*?)\)\s*'
        r'list\(APPEND ESP_VISION_MODULE_SOURCES(?P<body>.*?)\)\s*'
        r'endif\(\)',
        text,
        flags=re.DOTALL,
    )
    for match in conditional_blocks:
        condition_targets = set(
            re.findall(r'IDF_TARGET\s+STREQUAL\s+"([^"]+)"', match.group('condition'))
        )
        block_modules = _modules_from_source_block(match.group('body'))
        for target in condition_targets:
            if target in modules:
                modules[target].update(block_modules)

    return {
        target: tuple(module for module in DOCUMENTED_MODULES if module in modules[target])
        for target in TARGETS
    }


def _read_board_targets():
    boards = {target: [] for target in TARGETS}
    for path in sorted(MP_BOARD_ROOT.glob('*/port/mpconfigboard.cmake')):
        board_name = path.parent.parent.name
        if board_name == 'TEMPLATE':
            continue
        text = path.read_text(encoding='utf-8')
        match = re.search(r'set\(IDF_TARGET\s+([A-Za-z0-9_]+)\)', text)
        if match and match.group(1) in boards:
            boards[match.group(1)].append(board_name)
    return {target: tuple(names) for target, names in boards.items()}


def _barcode_enabled(board):
    path = BOARD_ROOT / board / 'board.cmake'
    if not path.exists():
        return False
    text = path.read_text(encoding='utf-8')
    return bool(re.search(r'set\(ESP_VISION_ENABLE_BARCODE\s+ON\)', text))


_MODULE_TARGETS = _read_module_targets()
_BOARD_TARGETS = _read_board_targets()

TARGET_CAPABILITIES = {
    target: {
        'modules': _MODULE_TARGETS[target],
        'boards': _BOARD_TARGETS[target],
        'barcode_boards': tuple(
            board for board in _BOARD_TARGETS[target] if _barcode_enabled(board)
        ),
    }
    for target in TARGETS
}

for _target, _capabilities in TARGET_CAPABILITIES.items():
    if not _capabilities['boards']:
        raise RuntimeError(
            'No MicroPython board maps to documentation target {!r}'.format(_target)
        )

# A symbol is included in target-level documentation only when all current
# boards for that target enable the board-level feature.
_barcode_targets = tuple(
    target for target in TARGETS
    if TARGET_CAPABILITIES[target]['boards']
    and (TARGET_CAPABILITIES[target]['barcode_boards']
         == TARGET_CAPABILITIES[target]['boards'])
)

SYMBOL_TARGETS = {
    'image': {
        'classes': {
            'barcode': _barcode_targets,
        },
        'methods': {
            'Image.find_barcodes': _barcode_targets,
        },
        'constants': {
            name: _barcode_targets
            for name in (
                'EAN2', 'EAN5', 'EAN8', 'UPCE', 'ISBN10', 'UPCA', 'EAN13',
                'ISBN13', 'I25', 'DATABAR', 'DATABAR_EXP', 'CODABAR',
                'CODE39', 'PDF417', 'CODE93', 'CODE128',
            )
        },
    },
}

# Standard MicroPython modules are controlled separately by mpconfigport.h,
# board mpconfigboard.h overrides, ESP-IDF version checks, and SoC capability
# macros. They cannot be filtered accurately from the target alone.
STANDARD_PORT_CONFIG_SOURCES = (
    MP_CONFIG_PORT,
    MP_BOARD_ROOT,
)


def module_targets(module):
    """Return targets whose firmware build includes ``module``."""
    return tuple(
        target for target in TARGETS
        if module in TARGET_CAPABILITIES[target]['modules']
    )


def only_expression(targets):
    """Return a Sphinx ``only`` expression for a sequence of target tags."""
    return ' or '.join(targets)


def unavailable_module_pages(target):
    """Return API page paths for modules not compiled for ``target``."""
    available = TARGET_CAPABILITIES[target]['modules']
    return [
        'api-reference/{}.rst'.format(module)
        for module in DOCUMENTED_MODULES
        if module not in available
    ]
