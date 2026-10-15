# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import base64
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('node_rom', ROOT / 'host/device-core/src/node_rom.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class HostRomAdapterTests(unittest.TestCase):
    def test_rom_process_uses_bytes_source_and_preserves_application_write_contract(self):
        calls = []
        esp = SimpleNamespace(CHIP_NAME='ESP32-P4', get_chip_revision=lambda: 302,
                              get_security_info=lambda: {'flags': 0, 'flash_crypt_cnt': 0},
                              change_baud=lambda value: calls.append(('baud', value)),
                              read_flash=lambda *_: b'AB', hard_reset=lambda: calls.append('reset'),
                              _port=SimpleNamespace(close=lambda: calls.append('close')))

        def write_flash(loader, files, **options):
            self.assertIs(loader, esp)
            self.assertEqual(files, [(65536, b'AB')])
            self.assertIsInstance(files[0][1], bytes)
            self.assertEqual(options, {'flash_mode': 'keep', 'flash_freq': 'keep', 'flash_size': 'keep', 'compress': True})
            calls.append('write')

        tool = SimpleNamespace(__version__='5.4.0', detect_chip=lambda **_: esp,
                               run_stub=lambda loader: loader, attach_flash=lambda _: None,
                               write_flash=write_flash)
        operations = [('initialize', []), ('security', []), ('stub', []),
                      ('write', [65536, base64.b64encode(b'AB').decode()]),
                      ('read', [65536, 2]), ('reset', [])]
        requests = '\n'.join(json.dumps({'id': index, 'method': method, 'args': args})
                             for index, (method, args) in enumerate(operations, 1))
        output = io.StringIO()
        with patch.dict(sys.modules, {'esptool': tool}), patch.object(sys, 'stdin', io.StringIO(requests)), patch.object(sys, 'stdout', output), patch.object(sys, 'argv', ['node_rom.py', 'COM3']):
            MODULE.main()
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertTrue(all('error' not in response for response in responses))
        self.assertEqual(responses[0]['result']['chipRevision'], 302)
        self.assertEqual(calls, [('baud', 460800), 'write', 'reset', 'close'])
