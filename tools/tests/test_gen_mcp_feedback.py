# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Regression coverage from camera/LCD application development feedback."""
from pathlib import Path
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'docs'))
import gen_mcp  # noqa: E402


class KnowledgeFeedbackTests(unittest.TestCase):
    def test_display_alias_and_methods_preserve_canonical_contract(self):
        module = gen_mcp._extract_espvision_module('display')
        symbols = {s['name']: s for s in module['symbols']}
        self.assertEqual(len(symbols), len(module['symbols']))
        for name in ('Display', 'Display.write', 'Display.width', 'Display.deinit'):
            with self.subTest(name=name):
                alias = symbols[name]
                canonical = symbols[name.replace('Display', 'ESP32Display', 1)]
                self.assertEqual(alias['aliasOf'], 'display.' + canonical['name'])
                self.assertEqual(alias['kind'], canonical['kind'])
                self.assertEqual(alias['targets'], canonical['targets'])
        self.assertTrue(symbols['Display']['signatures'][0].startswith('Display('))

    def test_histogram_contract_retains_fractional_return_type(self):
        symbols = {s['name']: s for s in gen_mcp._extract_espvision_module('image')['symbols']}
        bins = symbols['histogram.bins']
        self.assertEqual(bins['signatures'], ['bins() -> list[float]'])
        self.assertIn('normalized', bins['doc'])
        self.assertIn('1.0', bins['doc'])

    def test_return_types_do_not_require_ast_unparse_from_python_39(self):
        # The documentation builder runs Python 3.8, without ast.unparse.
        with mock.patch.object(gen_mcp.ast, 'unparse', None, create=True):
            symbols = {s['name']: s for s in gen_mcp._extract_espvision_module('image')['symbols']}
        self.assertEqual(symbols['histogram.bins']['signatures'], ['bins() -> list[float]'])

    def test_full_pack_uses_check_interpreter_and_runs_before_firmware_finishes(self):
        ci = (ROOT / '.gitlab/ci/docs.yml').read_text()
        job = ci.split('\nbuild_mcp_pack:\n', 1)[1]
        self.assertIn('  image: $CI_PYTHON_IMAGE\n', job)
        self.assertNotIn('$ESP_DOCS_BUILD_IMAGE', job)
        self.assertIn('    - job: mcp_pack_check\n', job)
        self.assertIn('    - job: evmux_host_checks\n', job)
        self.assertNotIn('job: firmware', job)

    def test_typing_placeholders_are_not_exported_as_runtime_aliases(self):
        names = {s['name'] for s in gen_mcp._extract_espvision_module('tflite')['symbols']}
        self.assertNotIn('TensorInput', names)
        self.assertNotIn('TensorOutput', names)


if __name__ == '__main__':
    unittest.main()
