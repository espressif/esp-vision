# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Keep release identity separate from the deployment repository allowlist."""
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class McpBridgeContractTests(unittest.TestCase):
    def test_bridge_passes_exact_release_identity(self):
        bridge = yaml.safe_load((ROOT / '.gitlab/ci/mcp.yml').read_text())['trigger_website_mcp_release']
        values = bridge['variables']
        self.assertEqual(values['ESP_VISION_MCP_PACKAGE_VERSION'], '$CI_COMMIT_TAG')
        self.assertEqual(values['ESP_VISION_MCP_PROJECT_ID'], '$CI_PROJECT_ID')
        self.assertEqual(values['RELEASE_EXPECTED_SOURCE_COMMIT'], '$CI_COMMIT_SHA')
        self.assertEqual(bridge['trigger']['strategy'], 'depend')

    def test_bridge_does_not_select_the_deployment_repository(self):
        bridge = yaml.safe_load((ROOT / '.gitlab/ci/mcp.yml').read_text())['trigger_website_mcp_release']
        self.assertNotIn('ESP_VISION_MCP_IMAGE_REPOSITORY', bridge['variables'])
        self.assertNotIn('MCP_ALLOWED_IMAGE_REPOSITORY', bridge['variables'])
        self.assertFalse(any('CI_REGISTRY' in value for value in bridge['variables'].values()))
