#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Explicit Linux hardware acceptance for native UVC, using the device CLI.

Requires ffmpeg, v4l2-ctl and Pillow. Select both serial and video devices
explicitly. The tests replace the UVC source and run a short camera program;
finally they restore the logo. They do not modify device files or MSC data.
"""

import argparse
import json
import os
from pathlib import Path
import io
import subprocess
import sys
import time

from preview_tcp import FrameParser

from PIL import Image, ImageStat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--video', required=True)
    parser.add_argument('--device-cli', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(out), TMP=str(out), TEMP=str(out))
    results = []

    # Keep the installed CLI as the device owner, recording its serial adapter's
    # incoming bytes to independently validate EV-MUX preview frames afterward.
    recorder = out / 'recording-python'
    recorder.write_text(f'#!{sys.executable}\n' + '''import os, signal, subprocess, sys
child = subprocess.Popen([sys.executable, *sys.argv[1:]],
                         stdin=sys.stdin, stdout=subprocess.PIPE, stderr=sys.stderr)
def stop(signum, frame):
    if child.poll() is None:
        child.terminate()
signal.signal(signal.SIGTERM, stop)
try:
    with open(os.environ["ESP_VISION_UVC_RX_RECORD"], "wb") as recording:
        while True:
            chunk = os.read(child.stdout.fileno(), 16384)
            if not chunk:
                break
            recording.write(chunk)
            offset = 0
            while offset < len(chunk):
                offset += os.write(sys.stdout.fileno(), chunk[offset:])
finally:
    if child.poll() is None:
        child.terminate()
    child.wait()
sys.exit(child.returncode)
''')
    recorder.chmod(0o700)

    def run(label, command, timeout=35, extra_env=None):
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                                env=dict(env, **(extra_env or {})))
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode or any(marker in result.stderr for marker in ('Traceback (most recent call last)', 'SyntaxError:')):
            raise RuntimeError(f'{label} failed; see {out / (label + ".log")}')
        return result.stdout

    def python(label, source):
        command = ['node', str(args.device_cli), 'python', '--port', args.port,
                   '--source', source, '--timeout', '25000', '--json']
        extra_env = None
        if label == 'camera-preview':
            command.extend(['--python', str(recorder)])
            extra_env = {'ESP_VISION_UVC_RX_RECORD': str(out / 'evmux-rx.bin')}
        response = run(label, command, extra_env=extra_env)
        payload = json.loads(response)
        assert payload['ok'], payload
        return payload

    def capture(label, frames=3):
        path = out / (label + '.mjpeg')
        log = (out / (label + '-video.log')).open('w')
        process = subprocess.Popen([
            'ffmpeg', '-hide_banner', '-loglevel', 'info', '-y', '-f', 'v4l2',
            '-input_format', 'mjpeg', '-video_size', '320x240', '-framerate', '10',
            '-i', args.video, '-frames:v', str(frames), '-c:v', 'copy', str(path),
        ], stdout=log, stderr=log, env=env)
        return process, log, path, frames

    def finish(record):
        process, log, path, expected_frames = record
        try:
            assert process.wait(timeout=15) == 0, path
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            log.close()
        data = path.read_bytes()
        # Decode every frame, including frames sent while the source changes.
        cursor = 0
        frames = []
        while cursor < len(data):
            start = data.find(b'\xff\xd8', cursor)
            end = data.find(b'\xff\xd9', start)
            assert start >= 0 and end > start, path
            frame = data[start:end + 2]
            with Image.open(io.BytesIO(frame)) as image:
                image.load()
                assert image.size == (320, 240), image.size
            frames.append(frame)
            cursor = end + 2
        assert len(frames) == expected_frames, (path, len(frames), expected_frames)
        jpg = path.with_suffix('.jpg')
        jpg.write_bytes(frames[-1])
        with Image.open(jpg) as image:
            assert image.size == (320, 240), image.size
            return ImageStat.Stat(image.convert('RGB').crop((100, 100, 180, 160))).mean

    def passed(name, detail=None):
        results.append({'name': name, 'status': 'passed', 'detail': detail})
        print('PASS', name, flush=True)

    active = None
    try:
        formats = run('formats', ['v4l2-ctl', '-d', args.video, '--list-formats-ext'])
        assert 'MJPG' in formats and '320x240' in formats, formats
        passed('enumeration')
        python('no-host', '''import uvc, image, gc
uvc.reset()
assert uvc.get_mode() == {"format":"MJPEG", "width":320, "height":240, "interval_us":100000}
assert not uvc.is_streaming()
assert uvc.write(image.Image(320, 240, image.RGB565)) is False
try:
    uvc.set_frame(image.Image(160, 120, image.RGB565))
except ValueError:
    pass
else:
    raise AssertionError("wrong dimensions accepted")
print("UVC_CONTRACT_OK")''')
        passed('no_host_and_invalid_inputs')
        active = capture('logo')
        rgb = finish(active)
        active = None
        passed('default_logo', rgb)
        python('fixed-copy', '''import uvc, image, gc
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(255,0,0),fill=True)
assert uvc.set_frame(img)
img.draw_rectangle((0,0,320,240),color=(0,255,0),fill=True)
del img
gc.collect()
print("UVC_FIXED_COPY_OK")''')
        active = capture('fixed-red')
        rgb = finish(active)
        active = None
        assert rgb[0] > 180 and rgb[1] < 60 and rgb[2] < 60, rgb
        passed('fixed_frame_owns_copy', rgb)
        active = capture('direct-blue', 30)
        python('direct-write', '''import uvc, image, time, gc
for _ in range(100):
    if uvc.is_streaming():
        break
    time.sleep_ms(20)
assert uvc.is_streaming()
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(0,0,255),fill=True)
assert uvc.write(img)
img.clear()
del img
gc.collect()
print("UVC_DIRECT_COPY_OK")''')
        rgb = finish(active)
        active = None
        assert rgb[2] > 180 and rgb[0] < 60 and rgb[1] < 60, rgb
        passed('direct_write_owns_copy_and_reopen', rgb)
        python('jpeg-input', '''import uvc, image, gc
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(0,255,0),fill=True)
jpg = img.to_jpeg(copy=True,quality=50)
assert uvc.set_frame(jpg)
del img, jpg
gc.collect()
try:
    uvc.bind("sensor")
except ValueError:
    pass
else:
    raise AssertionError("unknown source accepted")
print("UVC_JPEG_OK")''')
        active = capture('jpeg-green')
        rgb = finish(active)
        active = None
        assert rgb[1] > 180 and rgb[0] < 60 and rgb[2] < 60, rgb
        passed('jpeg_input_and_invalid_binding', rgb)
        python('oversize', '''import image, uvc, os
buf = bytearray(os.urandom(320*240*2))
img = image.Image(320,240,image.RGB565,buffer=buf)
jpg = img.to_jpeg(copy=True,quality=100)
assert jpg.size() > 48*1024
try:
    uvc.set_frame(jpg)
except ValueError:
    pass
else:
    raise AssertionError("oversized JPEG accepted")
print("UVC_OVERSIZE_OK")''')
        active = capture('preserved-green')
        rgb = finish(active)
        active = None
        assert rgb[1] > 180 and rgb[0] < 60 and rgb[2] < 60, rgb
        passed('oversized_frame_preserves_previous_source', rgb)
        active = capture('camera-preview', 50)
        python('camera-preview', '''import sensor, uvc, time
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=500)
uvc.bind("preview")
start = time.ticks_ms()
count = 0
while time.ticks_diff(time.ticks_ms(), start) < 3500:
    img = sensor.snapshot()
    img.draw_rectangle((80,80,120,100),color=(255,0,0),fill=True)
    img.flush()
    count += 1
assert count >= 5
print("UVC_EVMUX_CAMERA_OK", count)''')
        rgb = finish(active)
        active = None
        assert rgb[0] > 180 and rgb[1] < 80 and rgb[2] < 80, rgb
        passed('processed_camera_and_evmux_preview', rgb)

        parser = FrameParser()
        previews = []
        with (out / 'evmux-rx.bin').open('rb') as recording:
            while chunk := recording.read(16384):
                previews.extend(frame for frame in parser.feed(chunk)
                                if frame.metadata.get('channel') == 'preview.frame')
        assert parser.bad_frames == 0, parser.bad_frames
        assert len(previews) >= 3, len(previews)
        for frame in previews:
            assert frame.metadata.get('width') == 320 and frame.metadata.get('height') == 240
            with Image.open(io.BytesIO(frame.payload)) as image:
                image.load()
                assert image.size == (320, 240)
        (out / 'evmux-preview.jpg').write_bytes(previews[-1].payload)
        passed('evmux_preview_received_during_uvc', {'frames': len(previews), 'crc_errors': 0})
        # Repeated host reopen must continue delivering frames.
        for cycle in range(3):
            active = capture(f'reopen-{cycle}')
            finish(active)
            active = None
        passed('repeated_host_close_reopen')
        active = capture('camera-restarts', 40)
        python('camera-restarts', '''import sensor, uvc
for _ in range(3):
    sensor.shutdown(True)
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    img = sensor.snapshot()
    img.draw_rectangle((80,80,120,100),color=(255,0,0),fill=True)
    img.flush()
print("UVC_CAMERA_RESTART_OK")''')
        rgb = finish(active)
        active = None
        assert rgb[0] > 180 and rgb[1] < 80 and rgb[2] < 80, rgb
        passed('camera_restart_while_uvc_streaming', rgb)
        python('before-soft-reset', '''import uvc, image
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(255,0,0),fill=True)
assert uvc.set_frame(img)
print("UVC_BEFORE_RESET_OK")''')
        active = capture('vm-reset', 30)
        time.sleep(0.6)
        run('vm-soft-reset', ['node', str(args.device_cli), 'control', '--port', args.port,
                              '--action', 'reset', '--mode', 'soft', '--timeout', '8000', '--json'])
        finish(active)
        active = None
        assert (out / 'vm-reset.jpg').read_bytes() == (out / 'logo.jpg').read_bytes()
        passed('vm_soft_reset_keeps_uvc_and_restores_logo', {'decoded_frames': 30})
    except BaseException as exc:
        results.append({'name': 'acceptance', 'status': 'failed', 'error': str(exc)})
        raise
    finally:
        if active:
            active[0].kill()
            active[0].wait()
            active[1].close()
        try:
            python('cleanup', 'import uvc,sensor; uvc.reset(); sensor.shutdown(True); print("UVC_LOGO_RESTORED")')
        finally:
            (out / 'report.json').write_text(json.dumps({'results': results}, indent=2) + '\n')


if __name__ == '__main__':
    main()
