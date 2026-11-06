# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import base64
import contextlib
from collections import deque
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('node_rom', ROOT / 'host/device-core/src/node_rom.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
try:
    import esptool as REAL_ESPTOOL
    from esptool.loader import ESPLoader
except ImportError:
    REAL_ESPTOOL = ESPLoader = None


class HostRomAdapterTests(unittest.TestCase):
    def fixture(self, descriptors=(), failures=(), stub=False, port_name='COM3', enumeration_error=None, open_error=None):
        calls, connections = [], []
        failures = iter(failures)
        esp = SimpleNamespace(CHIP_NAME='ESP32-P4', IS_STUB=stub, get_chip_revision=lambda: 302,
                              read_mac=lambda: bytes.fromhex('e8f60ae75642'),
                              get_security_info=lambda: {'flags': 0, 'flash_crypt_cnt': 0},
                              secure_download_mode=False, get_flash_encryption_enabled=lambda: False,
                              get_secure_boot_enabled=lambda: False, uses_key_manager_for_flash_encryption=lambda: False,
                              erase_region=Mock(),
                              flash_md5sum=Mock(return_value=hashlib.md5(b'AB').hexdigest()),
                              change_baud=lambda value: calls.append(('baud', value)),
                              read_flash=lambda *_: b'AB', hard_reset=lambda: calls.append('reset'))

        esp.FLASH_SECTOR_SIZE = 4096
        esp.ESP_CMDS = {'READ_FLASH': 0xd2, 'ERASE_REGION': 0xd1}
        esp.check_command = Mock(return_value=0)

        def read_flash(offset, length):
            esp.check_command('read flash', esp.ESP_CMDS['READ_FLASH'],
                              struct.pack('<IIII', offset, length, esp.FLASH_SECTOR_SIZE, 64))
            return b'AB'
        esp.read_flash = read_flash

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
            loader.IS_STUB = True
            return loader

        def write_flash(loader, files, **options):
            self.assertIs(loader, esp)
            self.assertEqual(files, [(65536, b'AB')])
            self.assertIsInstance(files[0][1], bytes)
            self.assertEqual(options, {'flash_mode': 'keep', 'flash_freq': 'keep', 'flash_size': 'keep', 'compress': True})
            calls.append('write')
            self.assertEqual(loader.flash_md5sum(65536, 2), hashlib.md5(b'AB').hexdigest())

        def erase_region(loader, address, size, *, force):
            self.assertIs(loader, esp)
            self.assertIs(force, False)
            calls.append(('erase', address, size))
            return loader.erase_region(address, size)

        tool = SimpleNamespace(__version__='5.6.0', detect_chip=Mock(side_effect=detect_chip),
                               run_stub=run_stub, attach_flash=lambda _: calls.append('attach'),
                               write_flash=write_flash, erase_region=erase_region)
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
        self.assertEqual(responses[3]['result'], {'algorithm': 'md5', 'offset': 65536, 'bytes': 2,
                                                'digest': hashlib.md5(b'AB').hexdigest()})
        fixture.esp.flash_md5sum.assert_called_once_with(65536, 2)

    def test_write_receipt_requires_the_exact_verified_range_and_restores_method(self):
        for address, length in [(65536, 3), (65540, 2)]:
            with self.subTest(address=address, length=length):
                digest = Mock(return_value='1' * 32)
                esp = SimpleNamespace(flash_md5sum=digest)
                tool = SimpleNamespace(write_flash=lambda loader, *_args, **_kwargs: loader.flash_md5sum(address, length))
                self.assertIsNone(MODULE._write_flash(tool, esp, 65536, b'AB'))
                self.assertIs(esp.flash_md5sum, digest)
                digest.assert_called_once_with(address, length)

    def test_failed_official_write_cannot_return_a_receipt(self):
        digest = Mock(return_value='1' * 32)
        esp = SimpleNamespace(flash_md5sum=digest)

        def failed_write(loader, *_args, **_kwargs):
            loader.flash_md5sum(65536, 2)
            raise RuntimeError('MD5 mismatch')

        with self.assertRaisesRegex(RuntimeError, 'MD5 mismatch'):
            MODULE._write_flash(SimpleNamespace(write_flash=failed_write), esp, 65536, b'AB')
        self.assertIs(esp.flash_md5sum, digest)

    def test_write_restores_an_inherited_md5_method_without_shadowing_it(self):
        class Stub:
            def flash_md5sum(self, address, size):
                return hashlib.md5(b'AB').hexdigest()
        esp = Stub()
        tool = SimpleNamespace(write_flash=lambda loader, *_args, **_kwargs: loader.flash_md5sum(65536, 2))
        result = MODULE._write_flash(tool, esp, 65536, b'AB')
        self.assertEqual(result['digest'], hashlib.md5(b'AB').hexdigest())
        self.assertNotIn('flash_md5sum', vars(esp))

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
        fixture = self.fixture(stub=True)
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


    def test_read_rpc_forwards_default_and_explicit_packet_budgets(self):
        fixture = self.fixture(stub=True)
        original_check = fixture.esp.check_command
        responses = self.run_process(fixture, [('initialize', []), ('read', [65536, 2]),
            ('read', [65536, 2, {'blockSize': 4096, 'maxInFlight': 2}])])
        self.assertTrue(all('error' not in reply for reply in responses))
        self.assertEqual(responses[1]['result'], 'QUI=')
        self.assertEqual([struct.unpack('<IIII', call.args[2]) for call in original_check.call_args_list],
                         [(65536, 2, 1024, 1), (65536, 2, 4096, 2)])
        self.assertIs(fixture.esp.check_command, original_check)
        self.assertEqual(fixture.esp.FLASH_SECTOR_SIZE, 4096)

    def test_unsupported_esptool_version_is_rejected_before_opening_a_port(self):
        for version in ['5.3.1', '5.5.0', '5.6.1', '6.0.0']:
            fixture = self.fixture()
            fixture.tool.__version__ = version
            with self.assertRaisesRegex(RuntimeError, 'esptool==5.6.0'):
                self.run_process(fixture, [('initialize', [])])
            self.assertEqual(fixture.calls, [])
            self.assertEqual(fixture.ports, [])

    def test_non_stub_or_invalid_budget_never_issues_read_flash(self):
        fixture = self.fixture()
        with self.assertRaisesRegex(RuntimeError, 'flasher stub'):
            MODULE._read_flash(fixture.esp, 0, 2)
        fixture.esp.IS_STUB = True
        for options in [[], {'blockSize': True}, {'blockSize': None}, {'blockSize': 512}, {'maxInFlight': 0},
                        {'maxInFlight': 65}, {'maxInFlight': None}, {'maxInFlight': False}, {'unknown': 1}]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                MODULE._read_flash(fixture.esp, 0, 2, options)
        for offset, length in [(True, 2), (-1, 2), (0, 0), (0, 0x100000000), (0xffffffff, 2)]:
            with self.subTest(offset=offset, length=length), self.assertRaises(ValueError):
                MODULE._read_flash(fixture.esp, offset, length)
        fixture.esp.check_command.assert_not_called()

    def test_region_erase_rpc_uses_only_the_external_official_region_api(self):
        fixture = self.fixture()
        responses = self.run_process(fixture, [('initialize', []), ('stub', []),
            ('eraseRegion', [0x710000, 0x8f0000])])
        self.assertTrue(all('error' not in response for response in responses))
        self.assertEqual(responses[1]['result'], {'stubRunning': True})
        fixture.esp.erase_region.assert_called_once_with(0x710000, 0x8f0000)
        self.assertEqual(fixture.calls[-2:], [('erase', 0x710000, 0x8f0000), ('close', 1)])
        self.assertNotIn('write', fixture.calls)

    def test_region_erase_rejects_non_stub_or_malformed_ranges_before_external_erase(self):
        fixture = self.fixture()
        with self.assertRaisesRegex(RuntimeError, 'flasher stub'):
            MODULE._erase_region(fixture.tool, fixture.esp, 0x710000, 4096)
        fixture.esp.IS_STUB = True
        for address, size in [(-4096, 4096), (1, 4096), (0, 0), (0, 1), (0, 0x100000000),
                              (0xfffff000, 8192), (True, 4096), (0, True), (0x100000000, 4096)]:
            with self.subTest(address=address, size=size), self.assertRaises(ValueError):
                MODULE._erase_region(fixture.tool, fixture.esp, address, size)
        fixture.esp.erase_region.assert_not_called()
        fixture.esp.check_command.assert_not_called()

    def test_region_erase_failure_closes_the_port_and_seals_later_write_or_erase_requests(self):
        fixture = self.fixture(stub=True)
        fixture.esp.erase_region.side_effect = OSError('USB detached during erase')
        responses = self.run_process(fixture, [('initialize', []), ('eraseRegion', [0x710000, 4096]),
            ('write', [65536, 'QUI=']), ('eraseRegion', [0x710000, 4096])])
        self.assertEqual(responses[1]['error'], 'ROM_OPERATION_FAILED')
        self.assertEqual(responses[1]['detail'], 'USB detached during erase')
        for response in responses[2:]:
            self.assertEqual(response['error'], 'ROM_OPERATION_FAILED')
            self.assertIn('start a fresh ROM session', response['detail'])
        fixture.esp.erase_region.assert_called_once_with(0x710000, 4096)
        self.assertNotIn('write', fixture.calls)
        self.assertEqual(fixture.calls.count(('close', 1)), 1)

    def test_missing_stub_handshake_cannot_report_stub_success_or_erase(self):
        fixture = self.fixture()
        fixture.tool.run_stub = lambda esp: esp
        responses = self.run_process(fixture, [('initialize', []), ('stub', []), ('eraseRegion', [0x710000, 4096])])
        self.assertIn('validated flasher stub', responses[1]['detail'])
        self.assertIn('initialized flasher stub', responses[2]['detail'])
        fixture.esp.erase_region.assert_not_called()


@unittest.skipUnless(ESPLoader is not None, 'Install external esptool==5.6.0 for real upstream erase regressions')
class UpstreamRegionEraseTests(unittest.TestCase):
    def test_real_upstream_region_command_has_the_exact_range_opcode_and_scaled_timeout(self):
        self.assertEqual(REAL_ESPTOOL.__version__, '5.6.0')
        fixture = HostRomAdapterTests.fixture(self, stub=True)
        fixture.esp.erase_region = ESPLoader.erase_region.__get__(fixture.esp)
        with contextlib.redirect_stdout(io.StringIO()):
            MODULE._erase_region(REAL_ESPTOOL, fixture.esp, 0x710000, 0x8f0000)
        call = fixture.esp.check_command.call_args
        self.assertEqual(call.args, ('erase region', 0xd1, struct.pack('<II', 0x710000, 0x8f0000)))
        self.assertEqual(call.kwargs, {'timeout': max(3, 40 * (0x8f0000 / 1e6))})
        self.assertEqual(fixture.esp.check_command.call_count, 1)

    def test_real_upstream_security_checks_block_region_erase_without_force(self):
        self.assertEqual(REAL_ESPTOOL.__version__, '5.6.0')
        for feature in ['get_flash_encryption_enabled', 'get_secure_boot_enabled']:
            fixture = HostRomAdapterTests.fixture(self, stub=True)
            setattr(fixture.esp, feature, lambda: True)
            with self.subTest(feature=feature), self.assertRaisesRegex(Exception, 'Active security features'):
                MODULE._erase_region(REAL_ESPTOOL, fixture.esp, 0x710000, 4096)
            fixture.esp.erase_region.assert_not_called()


@unittest.skipUnless(ESPLoader is not None, 'Install external esptool==5.6.0 for real upstream reader regressions')
class UpstreamBoundedReadTests(unittest.TestCase):
    def fixture(self, mode=None):
        self.assertEqual(REAL_ESPTOOL.__version__, '5.6.0', 'Use the exact external reader qualified by production')
        payload = bytes(range(256)) * 20 + b'END!!'

        class Stub:
            IS_STUB = True
            FLASH_SECTOR_SIZE = 4096
            ESP_CMDS = ESPLoader.ESP_CMDS
            read_flash = ESPLoader.read_flash

            def __init__(inner):
                inner._port = SimpleNamespace(timeout=0)
                inner.commands, inner.acks, inner.frames = [], [], deque()

            def check_command(inner, description, opcode=None, data=b'', *args, **kwargs):
                inner.commands.append((description, opcode, data, args, kwargs))
                offset, length, block, window = struct.unpack('<IIII', data)
                for start in range(0, len(payload), block):
                    inner.frames.append(payload[start:start + block])
                inner.frames.append(hashlib.md5(payload).digest())
                if mode == 'short_packet':
                    inner.frames[0] = inner.frames[0][:512]
                if mode == 'wrong_digest':
                    inner.frames[-1] = b'\0' * 16
                if mode == 'missing_digest':
                    inner.frames[-1] = b''
                return 0

            def read(inner):
                if mode == 'timeout' and len(inner.acks) == 1:
                    raise TimeoutError('injected real-reader packet timeout')
                return inner.frames.popleft()

            def write(inner, data):
                inner.acks.append(struct.unpack('<I', data)[0])

        return Stub(), payload

    def assert_restored(self, stub):
        self.assertNotIn('check_command', vars(stub))
        self.assertNotIn('FLASH_SECTOR_SIZE', vars(stub))
        self.assertIs(stub.check_command.__func__, type(stub).check_command)
        self.assertEqual(stub.FLASH_SECTOR_SIZE, 4096)

    def test_real_upstream_method_receives_1024_one_window_and_validates_all_acks_and_md5(self):
        stub, expected = self.fixture()
        actual = MODULE._read_flash(stub, 0x10000, len(expected))
        self.assertEqual(actual, expected)
        self.assertEqual(len(stub.commands), 1)
        self.assertEqual(struct.unpack('<IIII', stub.commands[0][2]), (0x10000, len(expected), 1024, 1))
        self.assertEqual(stub.acks, [1024, 2048, 3072, 4096, 5120, len(expected)])
        self.assertEqual(len(stub.frames), 0, 'The original reader must consume its final digest')
        self.assert_restored(stub)

    def test_real_upstream_alternate_budget_keeps_the_original_4096_packet_parser(self):
        stub, expected = self.fixture()
        self.assertEqual(MODULE._read_flash(stub, 0, len(expected), {'blockSize': 4096, 'maxInFlight': 2}), expected)
        self.assertEqual(struct.unpack('<IIII', stub.commands[0][2]), (0, len(expected), 4096, 2))
        self.assertEqual(stub.acks, [4096, len(expected)])
        self.assert_restored(stub)

    def test_real_upstream_md5_short_packet_missing_digest_and_timeout_still_fail_without_retry(self):
        for mode, message in [('wrong_digest', 'Digest mismatch'), ('short_packet', 'Corrupt data'),
                              ('missing_digest', 'Expected digest'), ('timeout', 'injected real-reader packet timeout')]:
            with self.subTest(mode=mode):
                stub, expected = self.fixture(mode)
                with self.assertRaisesRegex(Exception, message):
                    MODULE._read_flash(stub, 0, len(expected))
                self.assertEqual(len(stub.commands), 1)
                self.assert_restored(stub)

    def test_existing_instance_overrides_are_restored_exactly_after_failure(self):
        stub, expected = self.fixture('wrong_digest')
        original = stub.check_command
        stub.check_command = original
        stub.FLASH_SECTOR_SIZE = 4096
        with self.assertRaisesRegex(Exception, 'Digest mismatch'):
            MODULE._read_flash(stub, 0, len(expected))
        self.assertIs(vars(stub)['check_command'], original)
        self.assertEqual(vars(stub)['FLASH_SECTOR_SIZE'], 4096)

    def test_different_range_or_unrecognized_layout_is_rejected_before_read_flash_is_sent(self):
        for alter in ['offset', 'length', 'window', 'short']:
            with self.subTest(alter=alter):
                stub, expected = self.fixture()
                def wrong_reader(offset, length):
                    fields = [offset, length, stub.FLASH_SECTOR_SIZE, 64]
                    if alter == 'offset':
                        fields[0] += 1
                    elif alter == 'length':
                        fields[1] += 1
                    elif alter == 'window':
                        fields[3] = 63
                    data = struct.pack('<IIII', *fields)
                    stub.check_command('read flash', stub.ESP_CMDS['READ_FLASH'], data[:8] if alter == 'short' else data)
                    return expected
                stub.read_flash = wrong_reader
                with self.assertRaisesRegex(RuntimeError, 'layout'):
                    MODULE._read_flash(stub, 0, len(expected))
                self.assertEqual(stub.commands, [])
                self.assert_restored(stub)

    def test_partial_compatibility_setup_failure_restores_the_packet_size(self):
        stub, expected = self.fixture()
        parent = type(stub)

        class RejectCheckOverride(parent):
            def __setattr__(inner, name, value):
                if name == 'check_command':
                    raise RuntimeError('injected check override failure')
                super().__setattr__(name, value)

        stub = RejectCheckOverride()
        with self.assertRaisesRegex(RuntimeError, 'injected check override failure'):
            MODULE._read_flash(stub, 0, len(expected))
        self.assertEqual(stub.commands, [])
        self.assert_restored(stub)

    def test_missing_or_duplicate_command_does_not_complete_a_claimed_bounded_read(self):
        for duplicate in [False, True]:
            with self.subTest(duplicate=duplicate):
                stub, expected = self.fixture()
                def unusual_reader(offset, length):
                    if duplicate:
                        for _ in range(2):
                            stub.check_command('read flash', stub.ESP_CMDS['READ_FLASH'],
                                struct.pack('<IIII', offset, length, stub.FLASH_SECTOR_SIZE, 64))
                    return expected
                stub.read_flash = unusual_reader
                with self.assertRaises(RuntimeError):
                    MODULE._read_flash(stub, 0, len(expected))
                self.assertEqual(len(stub.commands), 1 if duplicate else 0)
                self.assert_restored(stub)


if __name__ == '__main__':
    unittest.main()
