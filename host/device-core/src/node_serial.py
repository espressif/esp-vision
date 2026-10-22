# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Optional pyserial byte bridge. No EV-MUX or device operations live here."""
import json
import os
import signal
import sys
import threading

import serial


def _configure_serial_exclusive(port, platform=os.name):
    # pyserial implements this as a non-blocking advisory lock on POSIX. The
    # Windows backend does not support this option, so leave its behavior alone.
    if platform == 'posix':
        port.exclusive = True


def main():
    if sys.argv[1] == '--list':
        from serial.tools.list_ports import comports
        print(json.dumps([{'path': p.device, 'manufacturer': p.manufacturer,
                           'product': p.product, 'serialNumber': p.serial_number,
                           'vendorId': p.vid, 'productId': p.pid,
                           'location': p.location, 'hwid': p.hwid} for p in comports()]))
        return
    port = serial.Serial(port=None, baudrate=115200, timeout=0.01, write_timeout=2)
    # Configure before open to avoid deliberately asserting reset/boot straps.
    port.rts = False
    port.dtr = True
    port.port = sys.argv[1]
    _configure_serial_exclusive(port)
    stopped = threading.Event()

    def terminate(*_args):
        raise SystemExit(0)

    def receive():
        try:
            while not stopped.is_set():
                data = port.read(8192)
                if data:
                    offset = 0
                    while offset < len(data):
                        offset += os.write(1, data[offset:])
        except (OSError, serial.SerialException):
            if not stopped.is_set():
                os._exit(2)

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    try:
        port.open()
        thread = threading.Thread(target=receive, daemon=True)
        thread.start()
        print('READY', file=sys.stderr, flush=True)
        while data := sys.stdin.buffer.read1(8192):
            offset = 0
            while offset < len(data):
                written = port.write(data[offset:])
                if not written:
                    raise OSError('Zero-byte serial write')
                offset += written
    finally:
        stopped.set()
        port.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        text = str(error).lower()
        code = 'SERIAL_ACCESS_DENIED' if ('permission denied' in text or 'access is denied' in text or 'access denied' in text) else 'SERIAL_BUSY' if ('resource busy' in text or 'resource temporarily unavailable' in text or 'exclusively lock' in text) else 'SERIAL_ERROR'
        print(code, file=sys.stderr, flush=True)
        sys.exit(2)
