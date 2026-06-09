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

import os
import sys

from esp_docs.conf_docs import *  # noqa: F403, F401

# Keep a reference to ESP-Docs' own ``setup`` so our hook can chain to it
# instead of shadowing it (the wildcard import above brings it into scope).
_esp_docs_setup = globals().get('setup')

# Make the API generator (docs/gen_api.py) importable.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_api  # noqa: E402


def setup(app):
    # Regenerate docs/<lang>/api-reference/_generated/*.rst from stubs/*.pyi on
    # every build so the API reference signatures stay in sync with the code.
    gen_api.generate(app)
    if _esp_docs_setup is not None:
        return _esp_docs_setup(app)
    return None


# -- General configuration ---------------------------------------------------

# Supported documentation languages. Must contain at least the current
# project's language.
languages = ['en', 'zh_CN']

# The generated API fragments under api-reference/_generated/ are pulled into the
# curated module pages with ``.. include::``; exclude them so Sphinx does not
# also treat them as standalone documents (which would warn about missing
# toctree entries). ``.. include::`` still reads the raw files.
try:
    exclude_patterns  # noqa: F821 - may come from the wildcard import
except NameError:
    exclude_patterns = []
exclude_patterns += ['**/_generated/**']

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
