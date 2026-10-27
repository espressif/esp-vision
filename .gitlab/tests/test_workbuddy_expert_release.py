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
        self.assertIn('expose_as', job['artifacts'])
        self.assertIn('$CI_COMMIT_SHORT_SHA', job['artifacts']['name'])
        self.assertIn('$CI_PIPELINE_ID', job['artifacts']['name'])
        self.assertTrue(any('build:workbuddy-expert -- --ci' in line for line in job['script']))
        self.assertFalse(any('workbuddy_release.py' in line for line in job['script']))

    def test_only_protected_firmware_tags_publish_the_checked_artifact(self):
        config = yaml.safe_load((ROOT / '.gitlab/ci/plugin.yml').read_text())
        job = config['publish_workbuddy_expert']
        release_rule = yaml.safe_load((ROOT / '.gitlab/ci/rules.yml').read_text())['.rules:release']['rules'][0]['if']
        self.assertEqual(job['rules'], [{'if': '$CI_COMMIT_REF_PROTECTED == "true" && ' + release_rule}])
        self.assertEqual(job['stage'], 'post_deploy')
        needs = {item['job']: item['artifacts'] for item in job['needs']}
        self.assertEqual(needs['plugin_checks'], True)
        self.assertTrue({'qualify_firmware_release', 'upload-dl-trigger', 'release_source_order_checks', 'mcp_release_contract_checks'} <= needs.keys())
        self.assertFalse(any(name.startswith('publish_') for name in needs), 'Expert release must not depend on npm publication')
        commands = '\n'.join(job['script'])
        self.assertIn('workbuddy_release.py', commands)
        self.assertIn('--tag "$CI_COMMIT_TAG" --source-commit "$CI_COMMIT_SHA" --publish', commands)
        self.assertNotIn('npm ', commands)
        self.assertNotIn('build:workbuddy-expert', commands)

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
