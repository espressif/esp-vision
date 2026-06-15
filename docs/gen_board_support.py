# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Generate board support tables from firmware configuration files."""

import json
import re
from pathlib import Path

from gen_api import _write_if_changed
from targets import BOARD_ROOT, MP_BOARD_ROOT, MP_CONFIG_PORT, TARGET_CAPABILITIES

DOCS_DIR = Path(__file__).resolve().parent
LANGUAGES = ('en', 'zh_CN')

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
}

BOARD_URLS = {
    'ESP32_S3_EYE': (
        'https://github.com/espressif/esp-who/blob/master/docs/en/get-started/'
        'ESP32-S3-EYE_Getting_Started_Guide.md'
    ),
    'ESP32_S31_KORVO': (
        'https://docs.espressif.com/projects/esp-dev-kits/en/latest/'
        'esp32s31/esp32-s31-korvo-1/index.html'
    ),
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


def _board_info(board, target):
    metadata = json.loads(
        (MP_BOARD_ROOT / board / 'port' / 'board.json').read_text(encoding='utf-8')
    )
    port_macros = _defined_macros(MP_CONFIG_PORT)
    port_macros.update(_defined_macros(MP_BOARD_ROOT / board / 'port' / 'mpconfigboard.h'))
    imlib_macros = _defined_macros(BOARD_ROOT / board / 'imlib_config.h')
    modules = TARGET_CAPABILITIES[target]['modules']
    if board in TARGET_CAPABILITIES[target]['barcode_boards']:
        modules = modules + ('barcode',)
    board_cmake = BOARD_ROOT / board / 'board.cmake'
    requires_idf_master = (
        board_cmake.exists()
        and 'ESP_VISION_IDF_OVERLAY STREQUAL "master"'
        in board_cmake.read_text(encoding='utf-8')
    )
    return {
        'name': metadata['product'],
        'url': BOARD_URLS.get(board, metadata['url']),
        'chip': CHIP_NAMES[target],
        'image': BOARD_IMAGES[board],
        'modules': modules,
        'requires_idf_master': requires_idf_master,
        'port_features': tuple(
            label for macro, label in PORT_FEATURES if macro in port_macros
        ),
        'imlib_features': tuple(
            label
            for macros, label in IMLIB_GROUPS
            if all(macro in imlib_macros for macro in macros)
        ),
    }


def _render(lang):
    labels = {
        'en': {
            'picture': 'Picture',
            'board': 'Board',
            'chip': 'Chip',
            'support': 'ESP-VISION support',
            'supported': 'Supported',
            'idf_master': 'ESP-IDF master only',
            'modules': 'Modules',
            'micropython': 'MicroPython',
            'vision': 'Vision',
            'note': (
                'The support summary is generated from ``mpconfigport.h``, '
                'each board ``mpconfigboard.h`` and ``imlib_config.h``, and '
                'the chip-specific module selection.'
            ),
        },
        'zh_CN': {
            'picture': '图片',
            'board': '开发板',
            'chip': '芯片',
            'support': 'ESP-VISION 支持情况',
            'supported': '支持',
            'idf_master': '仅支持 ESP-IDF master',
            'modules': '模块',
            'micropython': 'MicroPython',
            'vision': '视觉能力',
            'note': (
                '支持摘要根据 ``mpconfigport.h``、各开发板的 '
                '``mpconfigboard.h`` 与 ``imlib_config.h``，以及芯片相关的'
                '模块选择自动生成。'
            ),
        },
    }[lang]
    out = [
        '.. This file is generated by docs/gen_board_support.py.',
        '.. Do not edit by hand; edit the firmware configuration instead.',
        '',
        '.. list-table::',
        '   :header-rows: 1',
        '   :widths: 20 20 12 48',
        '',
        '   * - {}'.format(labels['picture']),
        '     - {}'.format(labels['board']),
        '     - {}'.format(labels['chip']),
        '     - {}'.format(labels['support']),
    ]
    for target, capabilities in TARGET_CAPABILITIES.items():
        for board in capabilities['boards']:
            info = _board_info(board, target)
            status = labels['supported']
            if info['requires_idf_master']:
                status = '{} ({})'.format(status, labels['idf_master'])
            support = (
                '**{supported}** | {modules}: ``{module_values}`` | '
                '{micropython}: {port_values} | {vision}: {vision_values}'
            ).format(
                supported=status,
                modules=labels['modules'],
                module_values='``, ``'.join(info['modules']),
                micropython=labels['micropython'],
                port_values=', '.join(info['port_features']),
                vision=labels['vision'],
                vision_values=', '.join(info['imlib_features']),
            )
            out.extend(
                [
                    '   * - .. image:: {}'.format(info['image']),
                    '          :width: 180px',
                    '          :alt: {}'.format(info['name']),
                    '     - `{} <{}>`__'.format(info['name'], info['url']),
                    '     - ``{}``'.format(info['chip']),
                    '     - {}'.format(support),
                ]
            )
    out.extend(['', '.. note::', '', '   {}'.format(labels['note']), ''])
    return '\n'.join(out)


def generate(app=None):  # noqa: ARG001
    for lang in LANGUAGES:
        out_dir = DOCS_DIR / lang / 'target-support' / '_generated'
        out_dir.mkdir(parents=True, exist_ok=True)
        _write_if_changed(str(out_dir / 'boards.rst'), _render(lang))


if __name__ == '__main__':
    generate()
    print('Generated board support tables.')
