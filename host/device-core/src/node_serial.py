# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Optional pyserial byte bridge. No EV-MUX or device operations live here."""
import json
import os
import signal
import struct
import sys
import threading

import serial


def _report_serial_error(stage, error):
    """Emit bounded OS diagnostics, never exception text or transferred bytes."""
    number = getattr(error, 'errno', None)
    winerror = getattr(error, 'winerror', None)
    cause = error.__cause__ or error.__context__
    if not isinstance(number, int) and cause is not None:
        number = getattr(cause, 'errno', None)
    if not isinstance(winerror, int) and cause is not None:
        winerror = getattr(cause, 'winerror', None)
    def bounded_number(value):
        if isinstance(value, int) and not isinstance(value, bool) and -0x80000000 <= value <= 0xffffffff:
            return value
        return None
    number = bounded_number(number)
    winerror = bounded_number(winerror)
    # Pyserial sometimes wraps a Windows API failure without preserving errno.
    # Only recognize fixed API names/messages; arbitrary exception text may
    # contain application bytes and must not cross this diagnostic channel.
    text = str(error)[:512]
    message = 'Serial ' + stage + ' failed'
    if number is not None and 0 < number < 4096:
        message = os.strerror(number)
    else:
        for needle, safe in (
            ('ClearCommError', 'ClearCommError failed'),
            ('GetOverlappedResult', 'GetOverlappedResult failed'),
            ('ReadFile', 'ReadFile failed'),
            ('WriteFile', 'WriteFile failed'),
            ('readiness to read but returned no data', 'Device reports readiness to read but returned no data'),
            ('write timeout', 'Serial write timed out'),
            ('Zero-byte serial write', 'Serial write returned zero bytes'),
        ):
            if needle.lower() in text.lower():
                message = safe
                break
    record = {'kind': 'serial.error', 'stage': stage, 'type': type(error).__name__[:64],
              'errno': number, 'winerror': winerror, 'message': message[:160]}
    # One small line keeps READY/control records intact when the reader thread
    # fails concurrently with the main thread. stderr failure is terminal too.
    line = (json.dumps(record, ensure_ascii=True) + '\n').encode()
    try:
        offset = 0
        while offset < len(line):
            count = os.write(2, line[offset:])
            if count <= 0:
                break
            offset += count
    except OSError:
        pass


def _configure_serial_exclusive(port, platform=os.name):
    # pyserial implements this as a non-blocking advisory lock on POSIX. The
    # Windows backend does not support this option, so leave its behavior alone.
    if platform == 'posix':
        port.exclusive = True


def _read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            if data:
                raise OSError('Truncated serial bridge record')
            return None
        data.extend(chunk)
    return bytes(data)


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
        while not stopped.is_set():
            try:
                data = port.read(8192)
            except (OSError, serial.SerialException) as error:
                if not stopped.is_set():
                    _report_serial_error('read', error)
                    os._exit(2)
                return
            if data:
                try:
                    offset = 0
                    while offset < len(data):
                        offset += os.write(1, data[offset:])
                except OSError as error:
                    if not stopped.is_set():
                        _report_serial_error('stdout', error)
                        os._exit(2)
                    return

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    try:
        port.open()
        thread = threading.Thread(target=receive, daemon=True)
        thread.start()
        print('READY', file=sys.stderr, flush=True)
        while header := _read_exact(sys.stdin.buffer, 5):
            kind, size = struct.unpack('>BI', header)
            if kind not in (0, 1, 2) or size > 1048576:
                raise OSError('Invalid serial bridge record')
            data = _read_exact(sys.stdin.buffer, size)
            if data is None:
                raise OSError('Truncated serial bridge record')
            if kind == 1:
                request = json.loads(data)
                try:
                    baud = request['baudRate']
                    if not isinstance(baud, int) or isinstance(baud, bool) or baud <= 0:
                        raise ValueError('Invalid baudrate')
                    port.flush()
                    port.baudrate = baud
                    response = {'id': request['id'], 'ok': True, 'baudRate': baud}
                except (KeyError, ValueError, OSError, serial.SerialException):
                    response = {'id': request.get('id'), 'ok': False}
                print(json.dumps(response), file=sys.stderr, flush=True)
                continue
            if kind == 2:
                request = json.loads(data)
                try:
                    request_id = request['id']
                    if not isinstance(request_id, int) or isinstance(request_id, bool) or request_id <= 0:
                        raise ValueError('Invalid drain request ID')
                    # stdin records are processed in order. All preceding byte
                    # records have completed port.write before this drain ACK.
                    port.flush()
                    response = {'id': request_id, 'ok': True, 'drained': True}
                except (KeyError, ValueError, OSError, serial.SerialException):
                    response = {'id': request.get('id'), 'ok': False}
                print(json.dumps(response), file=sys.stderr, flush=True)
                continue
            try:
                offset = 0
                while offset < len(data):
                    written = port.write(data[offset:])
                    if not written:
                        raise OSError('Zero-byte serial write')
                    offset += written
            except (OSError, serial.SerialException) as error:
                _report_serial_error('write', error)
                raise SystemExit(2) from None
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
