# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Use a real host preprocessor to prove effective capability expressions."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('build_identity', ROOT / 'tools/ci/launchpad/build_identity.py')
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


class ReleaseBuildIdentityTests(unittest.TestCase):
    def test_compiler_evaluates_all_network_feature_gates(self):
        compiler = shutil.which('cc')
        if compiler is None:
            self.skipTest('Host compiler unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'platform').mkdir()
            (root / 'py').mkdir()
            source = root / 'platform/network_service.c'
            source.write_text('/* actual compiled source placeholder */')
            (root / 'app.bin').write_bytes(b'qualified image')
            (root / 'flasher_args.json').write_text('{"app":{"file":"app.bin"}}')
            (root / 'project_description.json').write_text('{"project_version":"v1"}')
            command = [compiler, '-I' + str(root), '-MD', '-MT', 'object', '-MF', str(root / 'object.d'),
                       '-o', str(root / 'object.o'), '-c', str(source)]
            (root / 'compile_commands.json').write_text(json.dumps([
                {'directory': str(root), 'file': str(source), 'arguments': command}]))
            for wlan, background, native, remote, expected in [
                (1, 1, 1, 0, True), (1, 1, 0, 1, True), (0, 1, 0, 1, False),
                (1, 0, 1, 0, False), (1, 1, 0, 0, False),
            ]:
                with self.subTest(wlan=wlan, background=background, native=native, remote=remote):
                    (root / 'py/mpconfig.h').write_text(f'#define MICROPY_PY_NETWORK_WLAN ({wlan})\n')
                    (root / 'boardconfig.h').write_text(
                        f'#define ESP_VISION_ENABLE_BACKGROUND_NETWORK ({background})\n'
                        f'#define CONFIG_ESP_WIFI_ENABLED ({native})\n'
                        f'#define CONFIG_ESP_WIFI_REMOTE_ENABLED ({remote})\n')
                    result = M.write_identity(root, 'BOARD', 'a' * 40)
                    self.assertEqual(result['capabilities']['wifi'], expected)
                    self.assertEqual(result['appSha256'], hashlib.sha256(b'qualified image').hexdigest())
                    self.assertFalse((root / 'object.o').exists())
                    self.assertFalse((root / 'object.d').exists())

    def test_missing_compile_provenance_fails_instead_of_guessing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'compile_commands.json').write_text('[]')
            with self.assertRaisesRegex(ValueError, 'actual network-service'):
                M.write_identity(root, 'BOARD', 'a' * 40)

    def test_repeated_sources_require_consistent_actual_compiler_contexts(self):
        compiler = shutil.which('cc')
        if compiler is None:
            self.skipTest('Host compiler unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'platform').mkdir()
            (root / 'py').mkdir()
            source = root / 'platform/network_service.c'
            source.write_text('/* INTERFACE source compiled by two targets */')
            (root / 'py/mpconfig.h').write_text('#define MICROPY_PY_NETWORK_WLAN (1)\n')
            (root / 'boardconfig.h').write_text('#define ESP_VISION_ENABLE_BACKGROUND_NETWORK (1)\n')
            (root / 'app.bin').write_bytes(b'qualified image')
            (root / 'flasher_args.json').write_text('{"app":{"file":"app.bin"}}')
            (root / 'project_description.json').write_text('{"project_version":"v1"}')
            command = [compiler, '-I' + str(root), '-DCONFIG_ESP_WIFI_ENABLED=1', '-c', str(source)]
            entries = [{'directory': str(root), 'file': str(source), 'arguments': command},
                       {'directory': str(root), 'file': str(source),
                        'arguments': [*command, '-DESP_PLATFORM', '-o', 'main.o']}]
            (root / 'compile_commands.json').write_text(json.dumps(entries))
            self.assertTrue(M.write_identity(root, 'BOARD', 'a' * 40)['capabilities']['wifi'])
            entries[1]['arguments'] = [argument.replace('CONFIG_ESP_WIFI_ENABLED=1', 'CONFIG_ESP_WIFI_ENABLED=0')
                                       for argument in entries[1]['arguments']]
            (root / 'compile_commands.json').write_text(json.dumps(entries))
            (root / 'release-build-identity.json').unlink()
            with self.assertRaisesRegex(ValueError, 'contexts disagree'):
                M.write_identity(root, 'BOARD', 'a' * 40)
            self.assertFalse((root / 'release-build-identity.json').exists())
            entries[1]['arguments'] = [*command, '-include', 'missing-capability-config.h']
            (root / 'compile_commands.json').write_text(json.dumps(entries))
            with self.assertRaisesRegex(ValueError, 'context 2 failed:.*missing-capability-config'):
                M.write_identity(root, 'BOARD', 'a' * 40)


if __name__ == '__main__':
    unittest.main()
