# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Internal JSON-line process adapter to separately installed esptool (GPL)."""
import base64
import contextlib
import json
import os
import struct
import sys


def _port_key(port):
    name = str(port)
    if name.startswith('\\\\.\\'):
        name = name[4:]
    if name.upper().startswith('COM') and name[3:].isdigit():
        return name.upper()
    return os.path.realpath(name)


def _reset_supported(port):
    # Descriptors permit a reset policy, never establish firmware/board identity.
    # Unknown/native USB ports must not receive the UART DTR/RTS sequence.
    try:
        from serial.tools.list_ports import comports
        matches = {(item.vid, item.pid) for item in comports()
                   if _port_key(item.device) == _port_key(port)}
    except (ImportError, OSError, ValueError):
        return False
    if len(matches) != 1:
        return False
    vid, pid = next(iter(matches))
    return vid in {0x10c4, 0x1a86, 0x0403, 0x067b} or (vid, pid) == (0x303a, 0x1001)


def _open_rom(esptool, port):
    import serial
    modes = ['no-reset', 'default-reset'] if _reset_supported(port) else ['no-reset']
    first_error = None
    for mode in modes:
        if mode == 'default-reset' and not _reset_supported(port):
            raise RuntimeError(f'Port no longer permits reset after synchronization failed: {first_error}') from first_error
        connection = serial.serial_for_url(port, baudrate=115200, exclusive=True, do_not_open=True)
        try:
            # Set before open on every OS, as esptool does on Windows. Opening
            # with asserted RTS can otherwise perturb an already-running stub.
            connection.rts = False
            connection.dtr = False
            connection.open()
            loader = esptool.detect_chip(port=connection, connect_mode=mode,
                                         connect_attempts=1 if mode == 'no-reset' else 7)
            return loader, mode
        except Exception as error:
            if connection.is_open:
                connection.close()
            if mode == modes[-1]:
                if first_error is not None:
                    raise RuntimeError(f'No-reset synchronization failed: {first_error}; reset connection failed: {error}') from error
                raise
            first_error = error



def _read_flash(esp, offset, length, options=None):
    """Bound the upstream stub reader without replacing its ACK/MD5 parser."""
    if type(offset) is not int or offset < 0 or type(length) is not int or length <= 0 or length > 0xffffffff or offset + length > 0x100000000:
        raise ValueError('ROM read requires a positive 32-bit address range')
    if options is None:
        options = {}
    if not isinstance(options, dict) or set(options) - {'blockSize', 'maxInFlight'}:
        raise ValueError('Unsupported ROM read options')
    block_size, max_in_flight = options.get('blockSize', 1024), options.get('maxInFlight', 1)
    if type(block_size) is not int or block_size not in (1024, 4096) or type(max_in_flight) is not int or not 1 <= max_in_flight <= 64:
        raise ValueError('ROM read requires 1024 or 4096 byte blocks and 1..64 outstanding packets')
    if not getattr(esp, 'IS_STUB', False):
        raise RuntimeError('Bounded Flash read requires an initialized flasher stub')
    if getattr(esp, 'FLASH_SECTOR_SIZE', None) != 4096 or not callable(getattr(esp, 'check_command', None)):
        raise RuntimeError('Unsupported esptool stub read layout; use esptool==5.6.0')
    command = esp.ESP_CMDS['READ_FLASH']
    original_check = esp.check_command
    missing = object()
    old_check = vars(esp).get('check_command', missing)
    old_block = vars(esp).get('FLASH_SECTOR_SIZE', missing)
    commands = 0
    block_changed = check_changed = False

    def bounded_check(op_description, op=None, data=b'', *args, **kwargs):
        nonlocal commands
        if op == command:
            commands += 1
            if commands != 1 or not isinstance(data, bytes) or len(data) != 16:
                raise RuntimeError('Unsupported esptool READ_FLASH command layout')
            wire_offset, wire_length, wire_block, wire_window = struct.unpack('<IIII', data)
            if (wire_offset, wire_length, wire_block, wire_window) != (offset, length, block_size, 64):
                raise RuntimeError('Unexpected esptool READ_FLASH range or packet layout')
            data = struct.pack('<IIII', offset, length, block_size, max_in_flight)
        return original_check(op_description, op, data, *args, **kwargs)

    try:
        # Upstream 5.6.0 uses this instance field for both its request and its
        # short-packet validation. Changing only the wire request is invalid.
        esp.FLASH_SECTOR_SIZE = block_size
        block_changed = True
        esp.check_command = bounded_check
        check_changed = True
        result = esp.read_flash(offset, length)
        if commands != 1 or not isinstance(result, bytes) or len(result) != length:
            raise RuntimeError('Upstream Flash read did not complete the bounded request')
        return result
    finally:
        try:
            if check_changed:
                if old_check is missing:
                    del esp.check_command
                else:
                    esp.check_command = old_check
        finally:
            if block_changed:
                if old_block is missing:
                    del esp.FLASH_SECTOR_SIZE
                else:
                    esp.FLASH_SECTOR_SIZE = old_block


def _erase_region(esptool, esp, address, size):
    """Erase only the qualified range through the external official command."""
    if type(address) is not int or address < 0 or address % 4096 or type(size) is not int or size <= 0 or size > 0xffffffff or size % 4096 or address + size > 0x100000000:
        raise ValueError('ROM erase requires a positive, sector-aligned 32-bit address range')
    if not getattr(esp, 'IS_STUB', False):
        raise RuntimeError('Flash region erase requires an initialized flasher stub')
    if getattr(esp, 'FLASH_SECTOR_SIZE', None) != 4096 or not callable(getattr(esp, 'erase_region', None)):
        raise RuntimeError('Unsupported esptool stub region erase; use esptool==5.6.0')
    # Do not use force: the official command also checks active security state.
    esptool.erase_region(esp, address, size, force=False)


def main():
    with contextlib.redirect_stdout(sys.stderr):
        import esptool
    if esptool.__version__ != '5.6.0':
        raise RuntimeError('Install esptool==5.6.0 for the verified bounded Flash reader')
    esp = None
    erase_failed = False
    families = {'ESP32-P4': 0x3280, 'ESP32-S3': 0x3253, 'ESP32-S31': 0x3231}
    try:
        for line in sys.stdin:
            request = json.loads(line)
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    method, args = request['method'], request.get('args', [])
                    result = None
                    if erase_failed:
                        raise RuntimeError('Flash region erase failed; start a fresh ROM session')
                    if method != 'initialize' and esp is None:
                        raise RuntimeError('Initialize the ROM adapter before issuing operations')
                    if method == 'initialize':
                        if esp is not None:
                            raise RuntimeError('ROM adapter is already initialized')
                        candidate, mode = _open_rom(esptool, sys.argv[1])
                        try:
                            mac = bytes(candidate.read_mac())
                            if len(mac) != 6:
                                raise RuntimeError('ROM did not return a six-byte factory MAC')
                            result = {'chipFamily': families.get(candidate.CHIP_NAME), 'chipRevision': candidate.get_chip_revision(),
                                      'connectionMode': mode, 'stubRunning': bool(candidate.IS_STUB), 'chipMac': mac.hex()}
                        except Exception:
                            if candidate._port.is_open:
                                candidate._port.close()
                            raise
                        esp = candidate
                    elif method == 'security':
                        security = esp.get_security_info()
                        result = {'flags': security['flags'], 'flashCryptCnt': security['flash_crypt_cnt']}
                    elif method == 'stub':
                        esp = esptool.run_stub(esp)
                        if not esp.IS_STUB:
                            raise RuntimeError('A validated flasher stub is required')
                        esptool.attach_flash(esp)
                        esp.change_baud(460800)
                        result = {'stubRunning': True}
                    elif method == 'flashId':
                        result = esp.flash_id()
                    elif method == 'read':
                        result = base64.b64encode(_read_flash(esp, *args)).decode('ascii')
                    elif method == 'eraseRegion':
                        _erase_region(esptool, esp, *args)
                    elif method == 'write':
                        offset, encoded = args
                        data = base64.b64decode(encoded, validate=True)
                        esptool.write_flash(esp, [(offset, data)], flash_mode='keep', flash_freq='keep', flash_size='keep', compress=True)
                    elif method == 'readRegister':
                        result = esp.read_reg(args[0])
                    elif method == 'writeRegister':
                        esp.write_reg(args[0], args[1], args[2])
                    elif method == 'reset':
                        esp.hard_reset()
                    else:
                        raise ValueError('Unknown ROM adapter method')
                print(json.dumps({'id': request['id'], 'result': result}), flush=True)
            except Exception as error:
                if request['method'] == 'eraseRegion':
                    erase_failed = True
                    with contextlib.suppress(Exception):
                        if esp is not None and esp._port.is_open:
                            esp._port.close()
                print(json.dumps({'id': request['id'], 'error': 'ROM_OPERATION_FAILED', 'method': request['method'], 'detail': str(error)}), flush=True)
    finally:
        if esp is not None and esp._port.is_open:
            esp._port.close()


if __name__ == '__main__':
    try:
        main()
    except Exception:
        print(json.dumps({'error': 'ESPTOOL_UNAVAILABLE'}), flush=True)
        sys.exit(1)
