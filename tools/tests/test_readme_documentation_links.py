# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('readme_links', ROOT / 'tools/ci/check_readme_links.py')
LINKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LINKS)


class DocumentationLinksTests(unittest.TestCase):
    def test_current_site_pages_resolve_to_versioned_sources(self):
        for lang in ('en', 'zh_CN'):
            url = f'https://docs.espressif.com/projects/esp-vision/{lang}/latest/esp32p4/device-core/api.html'
            self.assertEqual(LINKS.documentation_source(url, ROOT), ROOT / f'docs/{lang}/device-core/api.rst')

    def test_missing_generated_page_fails_local_link_validation(self):
        url = 'https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/missing-page.html'
        source = LINKS.documentation_source(url, ROOT)
        errors = LINKS.check_file_links([LINKS.Link(ROOT / 'README.md', str(source))])
        self.assertEqual(len(errors), 1)
        self.assertIsInstance(errors[0], LINKS.RelativeLinkError)

    def test_other_projects_and_published_versions_still_use_network_checks(self):
        for url in ('https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/device-core/api.html',
                    'https://docs.espressif.com/projects/esp-vision/en/v1.0/esp32p4/device-core/api.html',
                    'https://example.com/projects/esp-vision/en/latest/esp32p4/device-core/api.html'):
            self.assertIsNone(LINKS.documentation_source(url, ROOT))
