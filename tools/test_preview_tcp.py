#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Explicit TCP preview acceptance; replaces Python with bounded test programs.

Uses the installed device CLI for every device operation. Select the serial
control port and TCP host explicitly. Requires Pillow; --video also uses ffmpeg.
All artifacts, including subprocess temporary files, go under --output-dir.
"""

import argparse
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

from PIL import Image
from preview_tcp import PreviewObserver
from uvc_frames import decode_mjpeg_frames


class Reader:
    def __init__(self, observer, limit=None):
        self.observer = observer
        self.limit = limit
        self.frames = []
        self.errors = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        try:
            while not self.stop.is_set():
                frame = self.observer.receive(0.1)
                if frame:
                    assert frame.metadata.get('channel') == 'preview.frame', frame.metadata
                    self.frames.append(frame)
                    if self.limit is not None and len(self.frames) >= self.limit:
                        return
        except Exception as exc:
            self.errors.append(str(exc))

    def start(self):
        self.thread.start()

    def finish(self):
        self.stop.set()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive()
        assert not self.errors, {"errors": self.errors, "frames": len(self.frames),
                                 "badFrames": self.observer.parser.bad_frames,
                                 "last": self.frames[-1].metadata if self.frames else None}
        assert self.observer.parser.bad_frames == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--tcp-port', type=int, default=3232)
    parser.add_argument('--port', required=True, help='serial control port')
    parser.add_argument('--device-cli', type=Path, required=True)
    parser.add_argument('--video', help='optional UVC V4L2 node on the same device')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(out), TMP=str(out), TEMP=str(out))
    results = []
    observers = []
    active_readers = []

    def passed(name, detail=None):
        results.append(dict(name=name, status='passed', detail=detail))
        print('PASS', name, detail or '', flush=True)
        (out / 'report.json').write_text(json.dumps(results, indent=2))

    def cli(label, *command, timeout=45):
        result = subprocess.run(['node', str(args.device_cli), *command, '--json'],
                                capture_output=True, text=True, env=env, timeout=timeout)
        (out / f'{label}.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, f'{label}: {result.stderr}'
        assert 'Traceback (most recent call last)' not in result.stderr, result.stderr
        assert 'SyntaxError:' not in result.stderr, result.stderr
        response = json.loads(result.stdout)
        assert response['ok'], response
        return response

    def python(label, source):
        return cli(label, 'python', '--port', args.port, '--source', source, '--timeout', '40000')

    def connect():
        observer = PreviewObserver(args.host, args.tcp_port)
        observers.append(observer)
        observer.subscribe()
        return observer

    def check_frames(reader, label, minimum=10):
        reader.finish()
        active_readers.remove(reader)
        assert len(reader.frames) >= minimum, (label, len(reader.frames))
        sizes = set()
        previous = None
        for frame in reader.frames:
            meta = frame.metadata
            assert meta.get('contentType') == 'image/jpeg'
            assert 'sessionId' not in meta and 'vmEpoch' not in meta, meta
            if previous is not None:
                assert meta['seq'] > previous, (previous, meta)
            previous = meta['seq']
            with Image.open(io.BytesIO(frame.payload)) as image:
                image.load()
                assert image.size == (meta['width'], meta['height'])
                sizes.add(image.size)
        (out / f'{label}.jpg').write_bytes(reader.frames[-1].payload)
        return {'frames': len(reader.frames), 'badFrames': reader.observer.parser.bad_frames,
                'sizes': sorted(sizes)}

    def read(observer, limit=None):
        reader = Reader(observer, limit=limit)
        active_readers.append(reader)
        reader.start()
        return reader

    try:
        identity = cli('identity', 'inspect', '--port', args.port, '--fields', 'identity,network')
        passed('explicit_device_identity', identity['result']['identity'])
        python('initial-idle', 'import uvc,sensor\nuvc.reset()\nsensor.shutdown(True)\nprint("PREVIEW_IDLE_OK")')
        a, b = connect(), connect()
        # Longer than both the control lease and released-link idle timeout;
        # observers need no heartbeat and do not start sensor capture.
        deadline = time.monotonic() + 32
        while time.monotonic() < deadline:
            for observer in (a, b):
                unexpected = observer.receive(0.1)
                assert unexpected is None, (unexpected.metadata, unexpected.payload[:200])
        assert a.request('preview.subscribe')['ok']
        assert b.request('preview.subscribe')['ok']
        passed('two_idle_observers_without_capture_or_keepalive')
        third = PreviewObserver(args.host, args.tcp_port)
        observers.append(third)
        assert third.request('preview.subscribe')['error']['code'] == 'OBSERVER_LIMIT'
        third.close()
        cli('reserved-tcp-control', 'inspect', '--tcp', f'{args.host}:{args.tcp_port}', '--fields', 'identity')
        passed('observer_limit_and_reserved_tcp_control_slot')
        for method, payload in [('hello', {'evMuxVersion': 4, 'acquire': True, 'clientNonce': '0123456789abcdef0123456789abcdef'}), ('script.run', {}),
                                ('device.control', {'action': 'reset'})]:
            result = a.request(method, payload)
            assert result.get('ok') is False, result
            assert result['error']['code'] in ('READ_ONLY', 'SESSION_REQUIRED', 'NOT_OWNER'), result
        passed('observer_cannot_acquire_execute_or_reset')

        ra, rb = read(a), read(b)
        python('fixed-runtime-sizes', '''import image,time
for size in ((320,240),(160,120),(320,240)):
 img=image.Image(size[0],size[1],image.RGB565)
 img.draw_rectangle((0,0,size[0],size[1]),color=(0,255,0),fill=True)
 for _ in range(60):
  img.flush()
  time.sleep_ms(30)
print("PREVIEW_FIXED_OK")''')
        da, db = check_frames(ra, 'fixed-a'), check_frames(rb, 'fixed-b')
        assert len(da['sizes']) == len(db['sizes']) == 2
        common_a = {f.metadata['seq']: f.payload for f in ra.frames}
        common_b = {f.metadata['seq']: f.payload for f in rb.frames}
        common = common_a.keys() & common_b.keys()
        assert len(common) >= 20
        assert all(common_a[seq] == common_b[seq] for seq in common)
        passed('shared_frames_runtime_sizes_usb_control_no_sensor', [da, db, {'identicalFrames': len(common)}])

        # Release one fast observer, reconnect as a non-reading TCP consumer.
        assert b.request('preview.unsubscribe', timeout=12)['ok']
        b.close()
        # Set the receive window before SYN; shrinking it after connect can
        # leave a large advertised right edge and does not reliably stall TCP.
        slow_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        slow_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow_socket.settimeout(3)
        try:
            slow_socket.connect((args.host, args.tcp_port))
        except Exception:
            slow_socket.close()
            raise
        slow = PreviewObserver(args.host, args.tcp_port, connected_socket=slow_socket)
        observers.append(slow)
        slow.subscribe()
        slow_first = read(slow, limit=1)
        ra = read(a)
        video = video_log = None
        if args.video:
            video_log = (out / 'simultaneous-uvc.log').open('w')
            video = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'v4l2', '-input_format', 'mjpeg', '-video_size', '320x240', '-framerate', '10',
                '-i', args.video, '-frames:v', '50', '-c:v', 'copy', str(out / 'simultaneous-uvc.mjpeg')],
                stdout=video_log, stderr=video_log, env=env)
        try:
            python('camera-slow-observer', '''import sensor,time,uvc
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
uvc.bind("preview")
t=time.ticks_ms();count=0
while time.ticks_diff(time.ticks_ms(),t)<14000:
 img=sensor.snapshot()
 img.draw_rectangle((100,100,80,60),color=(255,0,0),fill=True)
 img.flush();count+=1
uvc.reset()
sensor.shutdown(True)
print("PREVIEW_CAMERA_OK",count)''')
            check_frames(slow_first, 'slow-first', minimum=1)
            fast = check_frames(ra, 'camera-fast', minimum=80)
            if video:
                assert video.wait(timeout=12) == 0
                data = (out / 'simultaneous-uvc.mjpeg').read_bytes()
                frames = decode_mjpeg_frames(data, expected_frames=50)
                passed('simultaneous_uvc_and_tcp', {'uvcBytes': len(data), 'completeDecodedFrames': len(frames)})
        finally:
            if video and video.poll() is None:
                video.kill()
                video.wait()
            if video_log:
                video_log.close()
        # Drain buffered TCP bytes: a closed socket can still have old data.
        closed = False
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            try:
                slow.receive(0.2)
            except ConnectionError:
                closed = True
                break
        assert closed, 'non-reading observer was not disconnected within the TX deadline'
        passed('slow_observer_isolated_and_closed', fast)
        assert a.request('preview.unsubscribe', timeout=12)['ok']
        a.close()
        replacement = connect()
        rr = read(replacement)
        python('reconnect', '''import image,time
img=image.Image(160,120,image.RGB565)
for _ in range(30):
 img.flush()
 time.sleep_ms(40)
print("PREVIEW_RECONNECT_OK")''')
        passed('reconnect_generation_and_unsubscribe', check_frames(rr, 'reconnected'))
        second = connect()
        first_reader, second_reader = read(replacement), read(second)
        cli('tcp-controller-preview', 'python', '--tcp', f'{args.host}:{args.tcp_port}',
            '--source', 'import image,time\nimg=image.Image(160,120,image.RGB565)\n'
                        'for _ in range(60):\n img.flush()\n time.sleep_ms(40)\n'
                        'print("TCP_CONTROLLER_PREVIEW_OK")', '--timeout', '25000')
        passed('tcp_controller_and_two_readonly_observers',
               [check_frames(first_reader, 'tcp-controller-a'),
                check_frames(second_reader, 'tcp-controller-b')])
        second.close()
        replacement.close()
    finally:
        for reader in active_readers:
            reader.stop.set()
            reader.thread.join(timeout=2)
        for observer in observers:
            observer.close()
        # Restore the default UVC logo and release camera resources. Preserve
        # the original test failure if a reset also prevents cleanup.
        failed = sys.exc_info()[0] is not None
        try:
            python('cleanup', 'import uvc,sensor\nuvc.reset()\nsensor.shutdown(True)\nprint("PREVIEW_CLEANUP_OK")')
        except Exception:
            if not failed:
                raise


if __name__ == '__main__':
    main()
