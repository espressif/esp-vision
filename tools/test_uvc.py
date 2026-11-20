#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Explicit Linux hardware acceptance for native UVC, using the device CLI.

Requires ffmpeg, v4l2-ctl and Pillow. Select both serial and video devices
explicitly. Use a device CLI with the Python serial adapter (`--python`)
for recording the raw EV-MUX preview. The tests run a short camera program
and restore the logo afterward. They do not modify device files or MSC data.
"""

import argparse
from fractions import Fraction
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zlib

from preview_tcp import FrameParser, HEADER
from uvc_frames import decode_mjpeg_frames

from PIL import Image, ImageStat



# Lower bound of platform/preview.c's fixed metadata, before optional session
# fields are appended. One-digit numbers and an empty route are conservative.
_PREVIEW_METADATA_MIN = len(
    '{"sid":"preview","seq":0,"channel":"preview.frame",'
    '"type":"data","encoding":"binary","route":"",'
    '"contentType":"image/jpeg","ts_ms":0,"width":1,"height":1}')


def validate_evmux_preview(recording, *, allow_collector_tail=False, tail_report=None):
    """Strictly validate every complete preview; default to rejecting any tail."""
    parser = FrameParser()
    wire = recording.read_bytes()
    frames = parser.feed(wire)
    assert parser.bad_frames == 0, parser.bad_frames
    tail = {'bytes': 0, 'allowed': False}
    if parser.has_partial_frame():
        raw_tail = bytes(parser.buffer)
        header = HEADER.fullmatch(raw_tail)
        last = frames[-1].metadata if frames else {}
        # This tool closes its recorder after the camera command releases its
        # session. Only a header with *zero* body bytes, following that release
        # ACK, and too short to describe a production preview can be exempted.
        # No unknown preview metadata, JPEG payload or MJPEG tail is discarded.
        assert allow_collector_tail, 'Incomplete EV-MUX recording'
        assert header is not None, 'Collector tail contains metadata or payload bytes'
        metadata_len, payload_len = int(header[1]), int(header[2])
        assert 0 < metadata_len < _PREVIEW_METADATA_MIN, 'Collector tail may describe a preview'
        assert 0 <= payload_len <= 1024, 'Collector tail exceeds bounded control data'
        assert (last.get('channel'), last.get('type'), last.get('method')) == (
            'user.rpc', 'rsp', 'session.release'), 'Collector tail is not after release ACK'
        tail = {'bytes': len(raw_tail), 'allowed': True, 'headerHex': raw_tail.hex(),
                'metadataBytesAdvertised': metadata_len, 'payloadBytesAdvertised': payload_len,
                'retainedBodyBytes': 0, 'productionPreviewMetadataMinimum': _PREVIEW_METADATA_MIN,
                'reason': 'Explicit recorder close after release ACK; header-only fragment is shorter than production preview metadata; no JPEG bytes discarded'}
    previews = [frame for frame in frames if frame.metadata.get('channel') == 'preview.frame']
    assert len(previews) >= 3, len(previews)
    # FrameParser rejects mismatched nonzero CRCs; additionally disallow its
    # legacy CRC=0 exemption for every complete preview in this diagnostic.
    position = 0
    while match := HEADER.search(wire, position):
        h, p = int(match[1]), int(match[2])
        body = match.end()
        end = body + h + p + 1
        if end > len(wire):
            break
        metadata = json.loads(wire[body:body + h])
        if metadata.get('channel') == 'preview.frame':
            crc = int(match[3], 16)
            assert crc != 0 and crc == zlib.crc32(wire[body + h:end - 1]), 'Preview CRC must be nonzero and exact'
        position = end
    for frame in previews:
        assert frame.metadata.get('contentType') == 'image/jpeg'
        assert frame.metadata.get('width') == 320 and frame.metadata.get('height') == 240
    payloads = [frame.payload for frame in previews]
    decoded = decode_mjpeg_frames(b''.join(payloads), len(payloads))
    assert decoded == payloads, 'Strict decoder changed original preview bytes'
    if tail_report is not None:
        tail_report.update(tail)
    return previews


def parse_mode(value):
    try:
        dimensions, rate = value.split('@')
        width, height = map(int, dimensions.split('x'))
        fps = int(rate)
        if min(width, height) > 0 and fps in (10, 20, 30):
            return width, height, fps
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('expected WIDTHxHEIGHT@FPS with 10, 20 or 30 FPS')


def measured_fps(path, frames):
    time_base = None
    timestamps = []
    for line in path.read_text().splitlines():
        if line.startswith('#tb 0:'):
            time_base = Fraction(line.split(':', 1)[1].strip())
        elif line and not line.startswith('#'):
            timestamps.append(int(line.split(',')[2]))
    assert time_base and len(timestamps) == frames
    assert all(a < b for a, b in zip(timestamps, timestamps[1:])), path
    return round(float((frames - 1) / ((timestamps[-1] - timestamps[0]) * time_base)), 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--video', required=True)
    parser.add_argument('--device-cli', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--mode', type=parse_mode, action='append', default=[],
                        help='also test the specified advertised WIDTHxHEIGHT@FPS mode')
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

    def capture(label, frames=3, mode=(320, 240, 10), measure=False):
        width, height, fps = mode
        path = out / (label + '.mjpeg')
        log = (out / (label + '-video.log')).open('w')
        command = [
            'ffmpeg', '-hide_banner', '-loglevel', 'info', '-y', '-f', 'v4l2',
            '-input_format', 'mjpeg', '-video_size', f'{width}x{height}', '-framerate', str(fps),
            '-i', args.video, '-frames:v', str(frames), '-c:v', 'copy', str(path),
        ]
        if measure:
            command.extend(['-map', '0:v:0', '-frames:v', str(frames), '-c:v', 'copy',
                            '-f', 'framecrc', str(path.with_suffix('.framecrc'))])
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        return process, log, path, frames

    def finish(record, size=(320, 240), *, alternative_sizes=()):
        process, log, path, expected_frames = record
        try:
            assert process.wait(timeout=15) == 0, path
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            log.close()
        # Decode every frame, including frames sent while the source changes.
        frames = decode_mjpeg_frames(path.read_bytes(), expected_frames, size, alternative_sizes=alternative_sizes)
        jpg = path.with_suffix('.jpg')
        jpg.write_bytes(frames[-1])
        with Image.open(jpg) as image:
            assert image.size == size, image.size
            x, y = image.width // 2, image.height // 2
            return ImageStat.Stat(image.convert('RGB').crop((x - 10, y - 10, x + 10, y + 10))).mean

    def passed(name, detail=None):
        results.append({'name': name, 'status': 'passed', 'detail': detail})
        print('PASS', name, flush=True)

    active = None
    wait_for_stream = '''import uvc, time
deadline = time.ticks_add(time.ticks_ms(), 4000)
while not uvc.is_streaming():
    assert time.ticks_diff(deadline, time.ticks_ms()) > 0, "UVC did not start"
    time.sleep_ms(20)
'''
    try:
        formats = run('formats', ['v4l2-ctl', '-d', args.video, '--list-formats-ext'])
        assert 'MJPG' in formats and '320x240' in formats, formats
        passed('enumeration')
        python('no-host', '''import uvc, image
uvc.reset()
assert uvc.get_mode()["format"] == "MJPEG"
assert not uvc.is_streaming()
assert uvc.write(image.Image(320, 240, image.RGB565)) is False
assert uvc.set_frame(image.Image(160, 120, image.RGB565)) is False
print("UVC_CONTRACT_OK")''')
        passed('no_host_drops_submissions')
        active = capture('logo')
        rgb = finish(active)
        active = None
        passed('default_logo', rgb)
        active = capture('fixed-red', 30)
        python('fixed-copy', wait_for_stream + '''import image, gc
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(255,0,0),fill=True)
uvc.bind("preview")
assert uvc.set_frame(img)
img.draw_rectangle((0,0,320,240),color=(0,255,0),fill=True)
img.flush()
del img
gc.collect()
print("UVC_FIXED_COPY_OK")''')
        rgb = finish(active)
        active = None
        assert rgb[0] > 180 and rgb[1] < 60 and rgb[2] < 60, rgb
        passed('fixed_frame_owns_copy', rgb)
        active = capture('fixed-frame-closed')
        finish(active)
        active = None
        assert (out / 'fixed-frame-closed.jpg').read_bytes() == (out / 'logo.jpg').read_bytes()
        passed('fixed_frame_clears_on_stream_close')
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
        active = capture('jpeg-green', 30)
        python('jpeg-input', wait_for_stream + '''import image, gc
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
        rgb = finish(active)
        active = None
        assert rgb[1] > 180 and rgb[0] < 60 and rgb[2] < 60, rgb
        passed('jpeg_input_and_invalid_binding', rgb)
        active = capture('preserved-green', 30)
        python('oversize', wait_for_stream + r'''import image
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(0,255,0),fill=True)
assert uvc.set_frame(img)
data = bytes(img.to_jpeg(copy=True, quality=50).bytearray())
jpg = image.Image(320,240,image.JPEG,buffer=data[:2]+b'\xff\xfe\xc0\x02'+bytes(49152)+data[2:])
try:
    uvc.set_frame(jpg)
except ValueError:
    pass
else:
    raise AssertionError("oversized JPEG accepted")
print("UVC_OVERSIZE_OK")''')
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

        collector_tail = {}
        previews = validate_evmux_preview(
            out / 'evmux-rx.bin', allow_collector_tail=True, tail_report=collector_tail)
        (out / 'evmux-preview.jpg').write_bytes(previews[-1].payload)
        passed('evmux_preview_received_during_uvc', {
            'frames': len(previews), 'crc_errors': 0, 'strict_entropy_decoded': True,
            'collector_tail': collector_tail})
        # Repeated host reopen must continue delivering frames.
        for cycle in range(10):
            active = capture(f'reopen-{cycle}')
            finish(active)
            active = None
            assert (out / f'reopen-{cycle}.jpg').read_bytes() == (out / 'logo.jpg').read_bytes()
        passed('repeated_host_close_reopen', {'cycles': 10})
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
        active = capture('vm-reset', 100)
        python('before-soft-reset', '''import uvc, image, time
deadline = time.ticks_add(time.ticks_ms(), 4000)
while not uvc.is_streaming():
    assert time.ticks_diff(deadline, time.ticks_ms()) > 0, "UVC did not start"
    time.sleep_ms(20)
img = image.Image(320,240,image.RGB565)
img.draw_rectangle((0,0,320,240),color=(255,0,0),fill=True)
assert uvc.set_frame(img)
print("UVC_BEFORE_RESET_OK")''')
        # Inspect exercises the bootstrap replies and device control on the
        # same USB connection while the isochronous capture remains open.
        inspect = ['node', str(args.device_cli), 'inspect', '--port', args.port,
                   '--fields', 'runtime,network', '--probe', '--json']
        before = json.loads(run('active-stream-before-reset', inspect))['result']['runtime']
        assert before['vmReady'] and before['vmEpoch'], before
        run('vm-soft-reset', ['node', str(args.device_cli), 'control', '--port', args.port,
                              '--action', 'reset', '--mode', 'soft', '--timeout', '8000', '--json'])
        after = json.loads(run('active-stream-after-reset', inspect))['result']['runtime']
        assert after['vmReady'] and after['vmEpoch'] != before['vmEpoch'], after
        finish(active)
        active = None
        assert (out / 'vm-reset.jpg').read_bytes() == (out / 'logo.jpg').read_bytes()
        passed('vm_soft_reset_keeps_uvc_and_restores_logo', {
            'decoded_frames': 100, 'before_epoch': before['vmEpoch'], 'after_epoch': after['vmEpoch'],
        })
        for width, height in ((160, 120), (480, 480), (640, 480), (1280, 720)):
            label = f'input-{width}x{height}'
            active = capture(label, 30)
            python(label, wait_for_stream + f'''import image
img = image.Image({width},{height},image.RGB565)
img.draw_rectangle((0,0,{width},{height}),color=(255,0,0),fill=True)
assert uvc.write(img)
assert uvc.get_mode()["width"] == 320 and uvc.get_mode()["height"] == 240
print("UVC_APPLICATION_SIZE_OK")''')
            rgb = finish(active, (width, height), alternative_sizes=((320, 240),))
            active = None
            assert rgb[0] > 180 and rgb[1] < 60 and rgb[2] < 60, rgb
            passed('application_dimensions', [width, height])
        for width, height, fps in args.mode:
            label = f'mode-{width}x{height}-{fps}fps'
            frame_count = 4 * fps
            active = capture(label, frame_count, (width, height, fps), measure=True)
            python(label, wait_for_stream + f'''notifications = []
uvc.set_mode_callback(notifications.append)
try:
    time.sleep_ms(100)
    selected = uvc.get_mode()
    assert notifications == [selected], notifications
    assert (selected["width"], selected["height"], selected["interval_us"]) == ({width}, {height}, {1000000 // fps}), selected
finally:
    uvc.set_mode_callback(None)
import image
img = image.Image({width},{height},image.RGB565)
img.draw_rectangle((0,0,{width},{height}),color=(255,0,0),fill=True)
assert uvc.write(img)
print("UVC_NEGOTIATED_MODE_OK")''')
            rgb = finish(active, (width, height))
            active = None
            assert rgb[0] > 180 and rgb[1] < 60 and rgb[2] < 60, rgb
            passed('advertised_mode_decodes', dict(mode=[width, height, fps],
                   decoded_frames=frame_count, measured_fps=measured_fps(out / f'{label}.framecrc', frame_count)))
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
