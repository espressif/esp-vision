# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Keep model-source publication outside the DL/npm dependency cycle."""
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
DEPLOY = ROOT / '.gitlab/ci/deploy.yml'


def jobs(needs):
    return {item if isinstance(item, str) else item['job'] for item in needs}


class ReleaseSourceOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = yaml.safe_load(DEPLOY.read_text())
        cls.source = cls.config['push_to_github']
        cls.upload = cls.config['upload-dl-trigger']

    def test_source_and_upload_share_the_deploy_stage(self):
        self.assertEqual(self.source['stage'], 'deploy')
        self.assertEqual(self.upload['stage'], 'deploy')

    def test_release_source_waits_for_firmware_qualification(self):
        rules = yaml.safe_load((ROOT / '.gitlab/ci/rules.yml').read_text())
        release_condition = rules['.rules:release']['rules'][0]['if']
        rule = next(item for item in self.source['rules'] if item['if'] == release_condition)
        self.assertIn('qualify_firmware_release', jobs(rule['needs']))
        self.assertNotEqual(rule.get('when'), 'manual')

    def test_dl_waits_for_the_exact_source_commit_to_be_mirrored(self):
        self.assertTrue({'push_to_github', 'pack_launchpad', 'qualify_firmware_release'} <= jobs(self.upload['needs']))

    def test_manual_npm_publication_cannot_block_source_mirroring(self):
        for filename, publisher in [('host-tools.yml', 'publish_device_cli_npm'),
                                    ('plugin.yml', 'publish_plugin_npm')]:
            config = yaml.safe_load((ROOT / '.gitlab/ci' / filename).read_text())
            self.assertEqual(self.source['stage'], config[publisher]['stage'])
        for rule in self.source['rules']:
            self.assertFalse(any(name.startswith('publish_') for name in jobs(rule.get('needs', []))))
        self.assertTrue(any(item['if'] == '$CI_COMMIT_TAG' for item in self.source['rules']),
                        'Standalone SDK and other non-firmware tags must still be mirrored')

    def test_release_qualification_requires_the_source_order_regression(self):
        check = self.config['release_source_order_checks']
        self.assertEqual(check['stage'], 'check')
        self.assertTrue(any('.gitlab/tests' in command for command in check['script']))
        self.assertIn('release_source_order_checks', jobs(self.config['qualify_firmware_release']['needs']))


if __name__ == '__main__':
    unittest.main()
