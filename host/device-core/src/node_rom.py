# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Internal JSON-line process adapter to separately installed esptool (GPL)."""
import base64
import contextlib
import json
import os
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


def main():
    with contextlib.redirect_stdout(sys.stderr):
        import esptool
    major, minor = map(int, esptool.__version__.split('.')[:2])
    if major != 5 or minor < 3:
        raise RuntimeError('Install esptool>=5.3,<6')
    esp = None
    families = {'ESP32-P4': 0x3280, 'ESP32-S3': 0x3253, 'ESP32-S31': 0x3231}
    try:
        for line in sys.stdin:
            request = json.loads(line)
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    method, args = request['method'], request.get('args', [])
                    result = None
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
                        esptool.attach_flash(esp)
                        esp.change_baud(460800)
                    elif method == 'flashId':
                        result = esp.flash_id()
                    elif method == 'read':
                        result = base64.b64encode(esp.read_flash(*args)).decode('ascii')
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
                print(json.dumps({'id': request['id'], 'error': 'ROM_OPERATION_FAILED', 'method': request['method'], 'detail': str(error)}), flush=True)
    finally:
        if esp is not None:
            esp._port.close()


if __name__ == '__main__':
    try:
        main()
    except Exception:
        print(json.dumps({'error': 'ESPTOOL_UNAVAILABLE'}), flush=True)
        sys.exit(1)
