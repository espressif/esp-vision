# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Decode retained UVC MJPEG bytes with Pillow and mandatory strict FFmpeg."""

import io
import shutil
import subprocess

from PIL import Image, ImageFile


def _strict_decode_mjpeg(data, expected_frames):
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg is None:
        raise RuntimeError('Strict UVC JPEG validation requires ffmpeg on PATH')
    command = [
        ffmpeg, '-v', 'error', '-xerror', '-err_detect', 'explode',
        '-threads', '1', '-f', 'mjpeg', '-i', 'pipe:0',
        '-vsync', '0', '-progress', 'pipe:1', '-nostats', '-f', 'null', '-',
    ]
    try:
        result = subprocess.run(command, input=data, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,
                                timeout=max(30, min(300, expected_frames / 20)))
    except FileNotFoundError as exc:
        raise RuntimeError('Strict UVC JPEG validation requires ffmpeg on PATH') from exc
    except subprocess.TimeoutExpired as exc:
        raise ValueError('Strict FFmpeg JPEG validation timed out') from exc
    if result.returncode or result.stderr:
        details = result.stderr.decode('utf-8', errors='replace')[-2048:].strip()
        raise ValueError(f'Strict FFmpeg JPEG validation failed ({result.returncode}): {details}')
    progress = result.stdout.decode('ascii', errors='replace').splitlines()
    decoded = [int(line.split('=', 1)[1]) for line in progress if line.startswith('frame=')]
    if not decoded or decoded[-1] != expected_frames or 'progress=end' not in progress:
        raise ValueError(f'Strict FFmpeg JPEG validation did not decode all {expected_frames} frames')


def decode_mjpeg_frames(data, expected_frames, size=(320, 240)):
    """Return original JPEG bytes only after both decoders accept every frame."""
    if type(expected_frames) is not int or expected_frames <= 0:
        raise ValueError('A positive expected frame count is required')
    if ImageFile.LOAD_TRUNCATED_IMAGES:
        raise ValueError('Full UVC acceptance requires truncated-image loading to be disabled')
    cursor = 0
    frames = []
    while cursor < len(data):
        if data[cursor:cursor + 2] != b'\xff\xd8':
            raise ValueError(f'Missing JPEG SOI at stream byte {cursor}')
        end = data.find(b'\xff\xd9', cursor + 2)
        if end < 0:
            raise ValueError(f'Missing JPEG EOI at stream byte {cursor}')
        frame = data[cursor:end + 2]
        with Image.open(io.BytesIO(frame)) as image:
            image.load()
            if image.format != 'JPEG' or image.size != size:
                raise ValueError(f'Unexpected UVC image format/dimensions: {image.format} {image.size}')
        frames.append(frame)
        cursor = end + 2
    if len(frames) != expected_frames:
        raise ValueError(f'Expected {expected_frames} complete UVC frames; decoded {len(frames)}')
    _strict_decode_mjpeg(data, expected_frames)
    return frames
