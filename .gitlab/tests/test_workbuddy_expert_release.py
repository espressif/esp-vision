# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Expert candidates stay downloadable; publication remains behind firmware gates."""
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class ExpertReleaseContractTests(unittest.TestCase):
    def test_every_plugin_check_exports_a_source_bound_download_without_publication(self):
        job = yaml.safe_load((ROOT / '.gitlab/ci/plugin.yml').read_text())['plugin_checks']
        self.assertNotIn('rules', job)
        self.assertIn('.cache/workbuddy-expert/', job['artifacts']['paths'])
        self.assertIn('.cache/cursor-plugin/', job['artifacts']['paths'])
        self.assertTrue(any('build:cursor -- --output-dir' in line for line in job['script']))
        self.assertIn('expose_as', job['artifacts'])
        self.assertIn('$CI_COMMIT_SHORT_SHA', job['artifacts']['name'])
        self.assertIn('$CI_PIPELINE_ID', job['artifacts']['name'])
        self.assertTrue(any('build:workbuddy-expert -- --ci' in line for line in job['script']))
        self.assertFalse(any('workbuddy_release.py' in line for line in job['script']))

    def test_zip_publication_is_owned_by_github_not_gitlab(self):
        config = yaml.safe_load((ROOT / '.gitlab/ci/plugin.yml').read_text())
        self.assertNotIn('publish_workbuddy_expert', config)
        self.assertIn('publish_plugin_npm', config)
        workflow = yaml.safe_load((ROOT / '.github/workflows/plugin-release.yml').read_text())
        self.assertEqual(workflow['jobs']['attach']['needs'], 'package')
        self.assertEqual(workflow['jobs']['attach']['permissions'], {'contents': 'write'})

    def test_shared_publisher_is_reused_without_rewriting_knowledge_or_firmware_release(self):
        helper = (ROOT / 'tools/ci/mcp/workbuddy_release.py').read_text()
        self.assertIn('from release import Registry, publish, sha256', helper)
        self.assertIn("PACKAGE = 'esp-vision-workbuddy-expert'", helper)
        self.assertIn('publish(registry, version, files)', helper)
        self.assertIn("env.get('CI_COMMIT_REF_PROTECTED') != 'true'", helper)
        job = yaml.safe_load((ROOT / '.gitlab/ci/mcp.yml').read_text())['mcp_release_contract_checks']
        self.assertIn("python -m unittest discover -s tools/ci/mcp -p 'test_*.py'", job['script'])
        self.assertTrue((ROOT / 'tools/ci/mcp/test_workbuddy_release.py').is_file(),
                        'The shared CI discovery must include the real ZIP/registry publication regressions')
