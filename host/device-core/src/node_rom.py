# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Internal JSON-line process adapter to separately installed esptool (GPL)."""
import base64
import contextlib
import json
import sys


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
                    if method == 'initialize':
                        esp = esptool.detect_chip(port=sys.argv[1])
                        result = {'chipFamily': families.get(esp.CHIP_NAME), 'chipRevision': esp.get_chip_revision()}
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
