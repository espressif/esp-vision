# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Common (non-language-specific) configuration for Sphinx, shared by
# en/conf.py and zh_CN/conf.py. It is imported into each language-specific
# conf.py during the ESP-Docs build.
# type: ignore
# pylint: disable=wildcard-import
# pylint: disable=undefined-variable

from esp_docs.conf_docs import *  # noqa: F403, F401

# -- General configuration ---------------------------------------------------

# Supported documentation languages. Must contain at least the current
# project's language.
languages = ['en', 'zh_CN']

# The documentation is chip-agnostic: the Python API is identical across the
# supported ESP32-P4 and ESP32-S3 boards, so no per-target ``idf_targets`` slug
# is configured. This keeps ``build-docs build`` working without ``-t`` and
# produces a single "generic" build per language.

# Project metadata used by the ESP-Docs theme and the GitHub edit links.
project_slug = 'esp-vision'
project_homepage = 'https://github.com/espressif/esp-vision'
github_repo = 'espressif/esp-vision'

# -- HTML / PDF output -------------------------------------------------------

html_static_path = ['../_static']

# PDF output filename prefix; ESP-Docs appends language and version.
pdf_file_prefix = u'esp-vision'
