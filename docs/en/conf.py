# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# English-language Sphinx configuration for ESP-VISION. It pulls in the shared
# settings from conf_common.py and only overrides the English specifics.
# type: ignore
# pylint: disable=wildcard-import
# pylint: disable=undefined-variable

import datetime
import os
import sys

# Make the shared conf_common.py (one level up) importable.
sys.path.insert(0, os.path.abspath('..'))

from conf_common import *  # noqa: E402, F403, F401

project = u'ESP-VISION Programming Guide'
copyright = u'2026 - {}, Espressif Systems (Shanghai) Co., Ltd'.format(datetime.datetime.now().year)
language = 'en'
pdf_title = u'ESP-VISION Programming Guide'
