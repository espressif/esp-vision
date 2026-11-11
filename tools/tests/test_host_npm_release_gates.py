# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Check the independent SDK and firmware-tag npm release gates."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class HostNpmReleaseGateTests(unittest.TestCase):
    def test_core_and_cli_publish_automatically_from_protected_default_branch(self):
        for filename, job in [('device-core.yml', 'publish_device_core_npm'),
                              ('host-tools.yml', 'publish_device_cli_npm')]:
            with self.subTest(job=job):
                source = (ROOT / '.gitlab' / 'ci' / filename).read_text()
                block = re.search(r'^' + job + r':\n(.*?)(?=^\S|\Z)', source, re.M | re.S).group(1)
                self.assertNotRegex(block, r'(?m)^  when: manual$')
                self.assertIn('$CI_PIPELINE_SOURCE == "push"', block)
                self.assertIn('$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH', block)
                self.assertIn('$CI_COMMIT_REF_PROTECTED == "true"', block)
                self.assertIn('- when: never', block)

    def test_plugin_and_knowledge_are_release_tag_jobs(self):
        import yaml
        for filename, job in [('plugin.yml', 'publish_plugin_npm'),
                              ('mcp-runtime.yml', 'publish_knowledge_mcp_npm')]:
            with self.subTest(job=job):
                config = yaml.safe_load((ROOT / '.gitlab/ci' / filename).read_text())
                block = config[job]
                self.assertEqual(block['extends'], '.rules:release')
                self.assertEqual(block['stage'], 'post_deploy')
                self.assertNotEqual(block.get('when'), 'manual')
                self.assertIn('qualify_firmware_release', {x['job'] for x in block['needs']})
        cli = yaml.safe_load((ROOT / '.gitlab/ci/host-tools.yml').read_text())
        package = cli['package_device_cli_npm']
        self.assertTrue(any('host/device-cli ci ' in line for line in package['script']))
        core_need = next(x for x in package['needs'] if x['job'] == 'publish_device_core_npm')
        self.assertTrue(core_need['optional'])
        plugin = yaml.safe_load((ROOT / '.gitlab/ci/plugin.yml').read_text())
        self.assertIn('package_plugin_npm', {x['job'] for x in plugin['publish_plugin_npm']['needs']})
        self.assertTrue(any('package_plugin_release.mjs' in line for line in plugin['package_plugin_npm']['script']))


    def test_cli_publish_consumes_the_lock_verified_by_its_package_job(self):
        import yaml
        cli = yaml.safe_load((ROOT / '.gitlab/ci/host-tools.yml').read_text())
        package = cli['package_device_cli_npm']
        commands = package['script']
        resolve_index = next(i for i, line in enumerate(commands) if 'resolve_core_release_lock.mjs' in line)
        install_index = next(i for i, line in enumerate(commands) if 'host/device-cli ci ' in line)
        self.assertLess(resolve_index, install_index)
        self.assertIn('host/device-cli/package-lock.json', package['artifacts']['paths'])
        need = next(x for x in cli['publish_device_cli_npm']['needs'] if x['job'] == 'package_device_cli_npm')
        self.assertTrue(need['artifacts'])

    def test_candidate_plugin_checks_do_not_require_unpublished_registry_versions(self):
        import yaml
        plugin = yaml.safe_load((ROOT / '.gitlab/ci/plugin.yml').read_text())
        candidate = '\n'.join(plugin['plugin_release_checks']['script'])
        self.assertIn('install_host.mjs', candidate)
        self.assertIn('--dry-run --package host/esp-vision-plugin', candidate)
        release = '\n'.join(plugin['package_plugin_npm']['script'])
        self.assertIn('package_plugin_release.mjs', release)
        self.assertNotIn('install_host.mjs', release)

    def test_publishers_require_their_complete_host_regressions(self):
        for filename, publisher, required in [
            ('host-tools.yml', 'publish_device_cli_npm', ['package_device_cli_npm', 'device_cli_checks', 'evmux_host_checks']),
            ('plugin.yml', 'publish_plugin_npm', ['package_plugin_npm', 'device_cli_checks', 'evmux_host_checks']),
        ]:
            with self.subTest(publisher=publisher):
                source = (ROOT / '.gitlab/ci' / filename).read_text()
                block = re.search(r'^' + publisher + r':\n(.*?)(?=^\S|\Z)', source, re.M | re.S).group(1)
                for job in required:
                    self.assertIn('    - job: ' + job + '\n', block)


if __name__ == '__main__':
    unittest.main()
