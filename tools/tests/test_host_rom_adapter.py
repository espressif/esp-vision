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
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('node_rom', ROOT / 'host/device-core/src/node_rom.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class HostRomAdapterTests(unittest.TestCase):
    def fixture(self, descriptors=(), failures=(), stub=False, port_name='COM3', enumeration_error=None, open_error=None):
        calls, connections = [], []
        failures = iter(failures)
        esp = SimpleNamespace(CHIP_NAME='ESP32-P4', IS_STUB=stub, get_chip_revision=lambda: 302,
                              read_mac=lambda: bytes.fromhex('e8f60ae75642'),
                              get_security_info=lambda: {'flags': 0, 'flash_crypt_cnt': 0},
                              change_baud=lambda value: calls.append(('baud', value)),
                              read_flash=lambda *_: b'AB', hard_reset=lambda: calls.append('reset'))

        class Port:
            is_open = False
            rts = True
            dtr = True

            def open(inner):
                self.assertIs(inner.rts, False)
                self.assertIs(inner.dtr, False)
                calls.append(('open', len(connections)))
                if open_error:
                    raise open_error
                inner.is_open = True

            def close(inner):
                self.assertTrue(inner.is_open, 'Do not close a failed handle twice')
                inner.is_open = False
                calls.append(('close', connections.index(inner) + 1))

        def serial_for_url(port, **options):
            self.assertEqual(port, port_name)
            self.assertEqual(options, {'baudrate': 115200, 'exclusive': True, 'do_not_open': True})
            connection = Port()
            connections.append(connection)
            return connection

        def detect_chip(**options):
            connection = options['port']
            self.assertIs(connection, connections[-1])
            self.assertTrue(connection.is_open)
            mode = options['connect_mode']
            calls.append(('detect', mode, options['connect_attempts']))
            failure = next(failures, None)
            if failure:
                if callable(failure):
                    failure(connection)
                else:
                    raise failure
            esp._port = connection
            return esp

        def run_stub(loader):
            self.assertIs(loader, esp)
            calls.append(('stub', loader.IS_STUB))
            return loader

        def write_flash(loader, files, **options):
            self.assertIs(loader, esp)
            self.assertEqual(files, [(65536, b'AB')])
            self.assertIsInstance(files[0][1], bytes)
            self.assertEqual(options, {'flash_mode': 'keep', 'flash_freq': 'keep', 'flash_size': 'keep', 'compress': True})
            calls.append('write')

        tool = SimpleNamespace(__version__='5.4.0', detect_chip=Mock(side_effect=detect_chip),
                               run_stub=run_stub, attach_flash=lambda _: calls.append('attach'),
                               write_flash=write_flash)
        comports = Mock(return_value=list(descriptors), side_effect=enumeration_error)
        modules = {'esptool': tool, 'serial': SimpleNamespace(serial_for_url=serial_for_url),
                   'serial.tools': SimpleNamespace(), 'serial.tools.list_ports': SimpleNamespace(comports=comports)}
        return SimpleNamespace(calls=calls, ports=connections, esp=esp, tool=tool, modules=modules,
                               comports=comports, port_name=port_name)

    def run_process(self, fixture, operations):
        requests = '\n'.join(json.dumps({'id': index, 'method': method, 'args': args})
                             for index, (method, args) in enumerate(operations, 1))
        output = io.StringIO()
        with patch.dict(sys.modules, fixture.modules), patch.object(sys, 'stdin', io.StringIO(requests)), \
                patch.object(sys, 'stdout', output), patch.object(sys, 'argv', ['node_rom.py', fixture.port_name]):
            MODULE.main()
        return [json.loads(line) for line in output.getvalue().splitlines()]

    def descriptor(self, vid, pid, device='COM3'):
        return SimpleNamespace(vid=vid, pid=pid, device=device)

    def test_rom_process_uses_bytes_source_and_preserves_application_write_contract(self):
        fixture = self.fixture()
        operations = [('initialize', []), ('security', []), ('stub', []),
                      ('write', [65536, base64.b64encode(b'AB').decode()]),
                      ('read', [65536, 2]), ('reset', [])]
        responses = self.run_process(fixture, operations)
        self.assertTrue(all('error' not in response for response in responses))
        self.assertEqual(responses[0]['result'], {'chipFamily': 0x3280, 'chipRevision': 302,
                         'connectionMode': 'no-reset', 'stubRunning': False, 'chipMac': 'e8f60ae75642'})
        self.assertEqual(fixture.calls, [('open', 1), ('detect', 'no-reset', 1), ('stub', False),
                                        'attach', ('baud', 460800), 'write', 'reset', ('close', 1)])

    def test_existing_rom_on_native_usb_syncs_without_reset(self):
        fixture = self.fixture([self.descriptor(0x303a, 0x0012)])
        responses = self.run_process(fixture, [('initialize', [])])
        self.assertEqual(responses[0]['result']['connectionMode'], 'no-reset')
        self.assertEqual(fixture.calls, [('open', 1), ('detect', 'no-reset', 1), ('close', 1)])

    def test_existing_stub_stays_detected_without_reset_even_on_a_uart_bridge(self):
        fixture = self.fixture([self.descriptor(0x10c4, 0xea60)], stub=True)
        responses = self.run_process(fixture, [('initialize', []), ('security', []), ('stub', [])])
        self.assertTrue(responses[0]['result']['stubRunning'])
        self.assertIn(('stub', True), fixture.calls)
        self.assertEqual(fixture.tool.detect_chip.call_count, 1)
        self.assertFalse(any(call[:2] == ('detect', 'default-reset') for call in fixture.calls if isinstance(call, tuple)))

    def test_confirmed_uart_sync_failure_closes_then_uses_bounded_reset(self):
        fixture = self.fixture([self.descriptor(0x10c4, 0xea60)], [RuntimeError('no sync')])
        responses = self.run_process(fixture, [('initialize', [])])
        self.assertEqual(responses[0]['result']['connectionMode'], 'default-reset')
        self.assertEqual(fixture.calls, [('open', 1), ('detect', 'no-reset', 1), ('close', 1),
                                        ('open', 2), ('detect', 'default-reset', 7), ('close', 2)])

    def test_confirmed_usj_supports_windows_com_alias_and_reset_fallback(self):
        fixture = self.fixture([self.descriptor(0x303a, 0x1001, 'com3')], [RuntimeError('no sync')],
                               port_name='\\\\.\\COM3')
        responses = self.run_process(fixture, [('initialize', [])])
        self.assertEqual(responses[0]['result']['connectionMode'], 'default-reset')

    def test_native_rom_or_application_usb_failure_never_uses_reset_fallback(self):
        for pid in [0x0012, 0x0020, 0x0009, 0x4003]:
            with self.subTest(pid=pid):
                fixture = self.fixture([self.descriptor(0x303a, pid)], [RuntimeError('native sync failed')])
                response = self.run_process(fixture, [('initialize', [])])[0]
                self.assertEqual(response['error'], 'ROM_OPERATION_FAILED')
                self.assertEqual(response['method'], 'initialize')
                self.assertEqual(response['detail'], 'native sync failed')
                self.assertEqual(fixture.calls, [('open', 1), ('detect', 'no-reset', 1), ('close', 1)])

    def test_missing_unknown_or_conflicting_descriptors_never_authorize_reset(self):
        descriptors = [[], [self.descriptor(None, None)], [self.descriptor(0x9999, 1)],
                       [self.descriptor(0x10c4, 0xea60), self.descriptor(0x303a, 0x0012)]]
        for records in descriptors:
            with self.subTest(records=records):
                fixture = self.fixture(records, [RuntimeError('no sync')])
                self.assertEqual(self.run_process(fixture, [('initialize', [])])[0]['error'], 'ROM_OPERATION_FAILED')
                self.assertEqual(fixture.tool.detect_chip.call_count, 1)
                self.assertFalse(fixture.ports[0].is_open)

    def test_unavailable_descriptor_enumeration_is_conservative(self):
        fixture = self.fixture(failures=[RuntimeError('no sync')], enumeration_error=PermissionError('denied'))
        self.assertEqual(self.run_process(fixture, [('initialize', [])])[0]['detail'], 'no sync')
        self.assertEqual(fixture.tool.detect_chip.call_count, 1)

    def test_reset_authorization_is_rechecked_after_failed_sync(self):
        fixture = self.fixture(failures=[RuntimeError('no sync')])
        fixture.comports.side_effect = [[self.descriptor(0x10c4, 0xea60)], [self.descriptor(0x303a, 0x0012)]]
        response = self.run_process(fixture, [('initialize', [])])[0]
        self.assertEqual(response['error'], 'ROM_OPERATION_FAILED')
        self.assertIn('no longer permits reset', response['detail'])
        self.assertEqual(fixture.tool.detect_chip.call_count, 1)
        self.assertEqual(len(fixture.ports), 1)
        self.assertFalse(fixture.ports[0].is_open)

    def test_both_connection_errors_preserved_and_both_handles_closed(self):
        fixture = self.fixture([self.descriptor(0x1a86, 0x7523)],
                               [RuntimeError('no sync'), RuntimeError('reset failed')])
        response = self.run_process(fixture, [('initialize', [])])[0]
        self.assertIn('no sync', response['detail'])
        self.assertIn('reset failed', response['detail'])
        self.assertEqual([x for x in fixture.calls if x[0] == 'close'], [('close', 1), ('close', 2)])

    def test_upstream_already_closed_failed_handle_is_not_closed_twice(self):
        def failed(connection):
            connection.close()
            raise RuntimeError('upstream closed')
        fixture = self.fixture(failures=[failed])
        response = self.run_process(fixture, [('initialize', [])])[0]
        self.assertEqual(response['detail'], 'upstream closed')
        self.assertEqual(fixture.calls[-1], ('close', 1))

    def test_port_open_failure_does_not_attempt_sync_or_leave_a_handle(self):
        fixture = self.fixture(open_error=OSError('unplugged'))
        response = self.run_process(fixture, [('initialize', [])])[0]
        self.assertEqual(response['detail'], 'unplugged')
        fixture.tool.detect_chip.assert_not_called()
        self.assertFalse(fixture.ports[0].is_open)

    def test_repeated_initialization_is_rejected_without_replacing_or_leaking_port(self):
        fixture = self.fixture()
        responses = self.run_process(fixture, [('initialize', []), ('initialize', []), ('read', [65536, 2])])
        self.assertEqual(responses[1]['error'], 'ROM_OPERATION_FAILED')
        self.assertIn('already initialized', responses[1]['detail'])
        self.assertEqual(responses[2]['result'], 'QUI=')
        self.assertEqual(fixture.tool.detect_chip.call_count, 1)
        self.assertEqual(len(fixture.ports), 1)
        self.assertEqual(fixture.calls[-1], ('close', 1))

    def test_failed_initialization_cannot_proceed_to_write(self):
        fixture = self.fixture(failures=[RuntimeError('no sync')])
        responses = self.run_process(fixture, [('initialize', []), ('write', [65536, 'QUI='])])
        self.assertTrue(all(x['error'] == 'ROM_OPERATION_FAILED' for x in responses))
        self.assertIn('Initialize the ROM adapter', responses[1]['detail'])
        self.assertNotIn('write', fixture.calls)

    def test_invalid_mac_closes_connection_and_cannot_proceed_to_write(self):
        fixture = self.fixture()
        fixture.esp.read_mac = lambda: bytes(range(8))
        responses = self.run_process(fixture, [('initialize', []), ('write', [65536, 'QUI='])])
        self.assertIn('six-byte factory MAC', responses[0]['detail'])
        self.assertIn('Initialize the ROM adapter', responses[1]['detail'])
        self.assertNotIn('write', fixture.calls)
        self.assertEqual(fixture.calls[-1], ('close', 1))

    def test_failed_revision_qualification_closes_connection_without_accepting_writes(self):
        fixture = self.fixture()
        fixture.esp.get_chip_revision = Mock(side_effect=RuntimeError('Revision unavailable'))
        responses = self.run_process(fixture, [('initialize', []), ('write', [65536, 'QUI='])])
        self.assertEqual(responses[0]['detail'], 'Revision unavailable')
        self.assertIn('Initialize the ROM adapter', responses[1]['detail'])
        self.assertNotIn('write', fixture.calls)
        self.assertEqual(fixture.calls[-1], ('close', 1))


if __name__ == '__main__':
    unittest.main()
