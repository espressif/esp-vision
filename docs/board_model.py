# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Single source of derived board capability data for ESP-VISION.

The *raw* truth is the firmware configuration itself (``boards/<BOARD>/...``
config files plus the registries below). :func:`board_info` is the **one and
only** place that parses those files into a normalized capability record.

Every downstream renderer must consume :func:`board_info` rather than
re-parsing the firmware config or hand-copying the data:

  - ``docs/gen_board_support.py``     -> RST board support tables (docs build)
  - ``tools/ci/launchpad/generate_launchpad.py`` -> config.toml ``[website]``

Keeping the parsing here (and nowhere else) is what guarantees the docs tables
and the website board matrix can never drift apart: they are two renderings of
the same derived data.
"""

import json
import re

from targets import BOARD_ROOT, MP_BOARD_ROOT, MP_CONFIG_PORT, TARGET_CAPABILITIES

LOCAL_STATIC_PREFIX = '../../_static/'
WEBSITE_STATIC_PREFIX = (
    'https://raw.githubusercontent.com/espressif/esp-vision/master/docs/_static/'
)

BOARD_IMAGES = {
    'ESP32_P4X_EYE': (
        'https://raw.githubusercontent.com/espressif/esp-dev-kits/master/'
        'docs/_static/esp32-p4-eye/esp32-p4-eye-isometric.png'
    ),
    'ESP32_P4X_FUNCTION_EV_BOARD': (
        'https://raw.githubusercontent.com/espressif/esp-dev-kits/master/'
        'docs/_static/esp32-p4x-function-ev-board/'
        'esp32-p4x-function-ev-board-isometric_v1.6.png'
    ),
    'ESP32_S31_KORVO': (
        'https://raw.githubusercontent.com/espressif/esp-dev-kits/master/'
        'docs/_static/esp32-s31-korvo-1/esp32-s31-korvo-1-isometric.png'
    ),
    'ESP32_S3_EYE': (
        'https://raw.githubusercontent.com/espressif/esp-who/master/'
        'docs/_static/get-started/ESP32-S3-EYE-isometric.png'
    ),
    'ESP32_P4X_VISION': '../../_static/boards/ESP32_P4X_VISION/ESP32-P4X-VISION.png',
    'AtomS3R-M12': '../../_static/boards/AtomS3R-M12/AtomS3R-M12.png',
}

# Canonical (English) getting-started URL override; falls back to board.json's
# url. The website swaps /en/ <-> /zh_CN/ per locale.
BOARD_URLS = {
    'ESP32_S3_EYE': (
        'https://github.com/espressif/esp-who/blob/master/docs/en/get-started/'
        'ESP32-S3-EYE_Getting_Started_Guide.md'
    ),
    'ESP32_S31_KORVO': (
        'https://docs.espressif.com/projects/esp-dev-kits/en/latest/'
        'esp32s31/esp32-s31-korvo-1/index.html'
    ),
    'AtomS3R-M12': 'https://docs.m5stack.com/en/core/AtomS3R-M12',
}

CHIP_NAMES = {
    'esp32p4': 'ESP32-P4',
    'esp32s3': 'ESP32-S3',
    'esp32s31': 'ESP32-S31',
}

PORT_FEATURES = (
    ('MICROPY_PY_NETWORK', 'network'),
    ('MICROPY_PY_NETWORK_WLAN', 'WLAN'),
    ('MICROPY_PY_MACHINE_I2C', 'I2C'),
    ('MICROPY_PY_MACHINE_SPI', 'SPI'),
    ('MICROPY_PY_MACHINE_UART', 'UART'),
    ('MICROPY_PY_MACHINE_PWM', 'PWM'),
    ('MICROPY_PY_MACHINE_WDT', 'WDT'),
)

IMLIB_GROUPS = (
    (('IMLIB_ENABLE_BINARY_OPS', 'IMLIB_ENABLE_MATH_OPS'), 'pixel/math'),
    (
        (
            'IMLIB_ENABLE_MEAN',
            'IMLIB_ENABLE_MEDIAN',
            'IMLIB_ENABLE_MODE',
            'IMLIB_ENABLE_MIDPOINT',
            'IMLIB_ENABLE_MORPH',
            'IMLIB_ENABLE_GAUSSIAN',
            'IMLIB_ENABLE_LAPLACIAN',
            'IMLIB_ENABLE_BILATERAL',
        ),
        'filters',
    ),
    (
        (
            'IMLIB_ENABLE_FIND_LINES',
            'IMLIB_ENABLE_FIND_CIRCLES',
            'IMLIB_ENABLE_FIND_RECTS',
        ),
        'geometry',
    ),
    (('IMLIB_FIND_TEMPLATE',), 'template matching'),
    (('IMLIB_ENABLE_QRCODES',), 'QR code'),
    (('IMLIB_ENABLE_APRILTAGS',), 'AprilTag'),
)


def _defined_macros(path):
    text = path.read_text(encoding='utf-8')
    return set(re.findall(r'^\s*#define\s+([A-Za-z0-9_]+)\b', text, re.MULTILINE))


def _website_image(image):
    if image.startswith(LOCAL_STATIC_PREFIX):
        return WEBSITE_STATIC_PREFIX + image[len(LOCAL_STATIC_PREFIX):]
    return image


def board_info(board, target):
    """Normalize one board's firmware config into a capability record.

    This is the single derivation point; do not re-implement parsing elsewhere.
    """
    metadata = json.loads(
        (MP_BOARD_ROOT / board / 'port' / 'board.json').read_text(encoding='utf-8')
    )
    port_macros = _defined_macros(MP_CONFIG_PORT)
    port_macros.update(_defined_macros(MP_BOARD_ROOT / board / 'port' / 'mpconfigboard.h'))
    imlib_macros = _defined_macros(BOARD_ROOT / board / 'imlib_config.h')
    modules = TARGET_CAPABILITIES[target]['modules']
    if board in TARGET_CAPABILITIES[target]['barcode_boards']:
        modules = modules + ('barcode',)
    image = BOARD_IMAGES[board]
    return {
        'name': metadata['product'],
        'url': BOARD_URLS.get(board, metadata.get('url', '')),
        'chip': CHIP_NAMES[target],
        'image': image,
        'website_image': _website_image(image),
        'modules': modules,
        'port_features': tuple(
            label for macro, label in PORT_FEATURES if macro in port_macros
        ),
        'imlib_features': tuple(
            label
            for macros, label in IMLIB_GROUPS
            if all(macro in imlib_macros for macro in macros)
        ),
    }


def iter_boards():
    """Yield ``(target, board, board_info(...))`` for every configured board."""
    for target, capabilities in TARGET_CAPABILITIES.items():
        for board in capabilities['boards']:
            yield target, board, board_info(board, target)
