# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Simplified-Chinese Sphinx configuration for ESP-VISION. It pulls in the
# shared settings from conf_common.py and only overrides the Chinese specifics.
# type: ignore
# pylint: disable=wildcard-import
# pylint: disable=undefined-variable

import datetime
import os
import sys

# Make the shared conf_common.py (one level up) importable.
sys.path.insert(0, os.path.abspath('..'))

from conf_common import *  # noqa: E402, F403, F401

project = u'ESP-VISION 编程指南'
copyright = u'2026 - {}, 乐鑫信息科技(上海)股份有限公司'.format(datetime.datetime.now().year)
language = 'zh_CN'
pdf_title = u'ESP-VISION 编程指南'
