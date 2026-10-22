# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Merging host changes must not itself authorize public npm publication."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class HostNpmReleaseGateTests(unittest.TestCase):
    def test_public_host_jobs_require_manual_protected_default_branch_actions(self):
        jobs = [
            ('device-core.yml', 'publish_device_core_npm'),
            ('host-tools.yml', 'publish_device_cli_npm'),
            ('plugin.yml', 'publish_plugin_npm'),
        ]
        for filename, job in jobs:
            with self.subTest(job=job):
                source = (ROOT / '.gitlab' / 'ci' / filename).read_text()
                match = re.search(r'^' + job + r':\n(.*?)(?=^\S|\Z)', source, re.M | re.S)
                self.assertIsNotNone(match, job)
                block = match.group(1)
                self.assertRegex(block, r'(?m)^  when: manual$')
                self.assertRegex(block, r'(?m)^  allow_failure: false$')
                self.assertIn('$CI_PIPELINE_SOURCE == "push"', block)
                self.assertIn('$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH', block)
                self.assertIn('$CI_COMMIT_REF_PROTECTED == "true"', block)
                self.assertIn('- when: never', block)


    def test_publishers_require_their_complete_host_regressions(self):
        for filename, publisher, required in [
            ('host-tools.yml', 'publish_device_cli_npm', ['device_cli_checks', 'evmux_host_checks']),
            ('plugin.yml', 'publish_plugin_npm', ['plugin_checks', 'device_cli_checks', 'evmux_host_checks']),
        ]:
            with self.subTest(publisher=publisher):
                source = (ROOT / '.gitlab/ci' / filename).read_text()
                block = re.search(r'^' + publisher + r':\n(.*?)(?=^\S|\Z)', source, re.M | re.S).group(1)
                for job in required:
                    self.assertIn('    - job: ' + job + '\n', block)


if __name__ == '__main__':
    unittest.main()
