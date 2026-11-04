#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Read-only EV-MUX preview. Requires only Python; --view also needs ffplay.

    python tools/preview_tcp.py 192.168.1.100 --view
    python tools/preview_tcp.py 192.168.1.100 --duration 10 --save-last preview.jpg

The application must call img.flush(). Connecting never starts the sensor,
acquires a control session, or interrupts Python. Ctrl-C disconnects cleanly.
"""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time

from dataclasses import dataclass
import re
import zlib

HEADER = re.compile(rb"\x1eEVMUX/1 h=(\d+) p=(\d+) c=([0-9a-fA-F]{8})\r\n")

@dataclass(frozen=True)
class Frame:
    metadata: dict
    payload: bytes

    def json(self):
        return json.loads(self.payload)


def _could_be_header_prefix(data: bytes) -> bool:
    pos = 0

    def literal(token):
        nonlocal pos
        remaining = data[pos:]
        if len(remaining) < len(token):
            return token.startswith(remaining), True
        if not remaining.startswith(token):
            return False, False
        pos += len(token)
        return True, False

    matched, partial = literal(b"\x1eEVMUX/1")
    if not matched or partial:
        return matched
    for token in (b" h=", b" p="):
        matched, partial = literal(token)
        if not matched or partial:
            return matched
        start = pos
        while pos < len(data) and 48 <= data[pos] <= 57:
            pos += 1
        if pos == len(data):
            return True
        if pos == start:
            return False

    matched, partial = literal(b" c=")
    if not matched or partial:
        return matched
    start = pos
    while pos < len(data) and data[pos] in b"0123456789abcdefABCDEF":
        pos += 1
    if pos == len(data):
        return pos - start <= 8
    if pos - start != 8:
        return False
    matched, _ = literal(b"\r\n")
    return matched


class FrameParser:
    """Incrementally parse bounded frames; reject bad CRC and recover at SOF."""

    def __init__(self, metadata_max=4096, payload_max=256 * 1024, frame_timeout=12):
        self.buffer = bytearray()
        self.metadata_max = metadata_max
        self.payload_max = payload_max
        self.frame_timeout = frame_timeout
        self.started_at = None
        self.bad_frames = 0

    def has_partial_frame(self) -> bool:
        if self.started_at is not None:
            return True
        data = bytes(self.buffer)
        start = 0
        while True:
            start = data.find(b"\x1e", start)
            if start < 0:
                return False
            if _could_be_header_prefix(data[start:]):
                return True
            start += 1

    def feed(self, data: bytes, now=None) -> list[Frame]:
        now = time.monotonic() if now is None else now
        self.buffer.extend(data)
        frames = []
        while self.buffer:
            match = HEADER.search(self.buffer)
            if match is None:
                # Preserve a possible split header, with bounded garbage retention.
                if len(self.buffer) > 72:
                    del self.buffer[:-72]
                self.started_at = None
                break
            if match.start():
                del self.buffer[:match.start()]
                match = HEADER.match(self.buffer)
                self.started_at = None
            metadata_len, payload_len = int(match[1]), int(match[2])
            if not 0 < metadata_len <= self.metadata_max or payload_len > self.payload_max:
                self.bad_frames += 1
                del self.buffer[0]
                self.started_at = None
                continue
            end = match.end() + metadata_len + payload_len + 1
            if len(self.buffer) < end:
                if self.started_at is None:
                    self.started_at = now
                if now - self.started_at >= self.frame_timeout:
                    self.bad_frames += 1
                    del self.buffer[0]
                    self.started_at = None
                    continue
                break
            metadata_bytes = bytes(self.buffer[match.end():match.end() + metadata_len])
            payload = bytes(self.buffer[match.end() + metadata_len:end - 1])
            crc = int(match[3], 16)
            try:
                metadata = json.loads(metadata_bytes)
                valid = isinstance(metadata, dict) and self.buffer[end - 1] == 0x1f
                valid = valid and (crc == 0 or zlib.crc32(payload) == crc)
            except (ValueError, UnicodeError):
                valid = False
            if not valid:
                self.bad_frames += 1
                del self.buffer[0]
                self.started_at = None
                continue
            frames.append(Frame(metadata, payload))
            del self.buffer[:end]
            self.started_at = None
        return frames


def encode_frame(metadata: dict, payload: bytes, crc=True) -> bytes:
    encoded = json.dumps(metadata, separators=(",", ":"), ensure_ascii=False).encode()
    checksum = zlib.crc32(payload) if crc else 0
    header = f"\x1eEVMUX/1 h={len(encoded)} p={len(payload)} c={checksum:08X}\r\n".encode()
    return header + encoded + payload + b"\x1f"





class PreviewObserver:
    """Independent TCP socket; deliberately does not use the owner SDK hello."""

    def __init__(self, host, port=3232, timeout=3, *, connected_socket=None):
        # A supplied socket can configure its receive window before connect.
        self.socket = connected_socket if connected_socket is not None else socket.create_connection((host, port), timeout=timeout)
        self.parser = FrameParser()
        self.pending = collections.deque()
        self.seq = 0

    def close(self):
        self.socket.close()

    def receive(self, timeout=1):
        if self.pending:
            return self.pending.popleft()
        self.socket.settimeout(timeout)
        try:
            data = self.socket.recv(65536)
        except socket.timeout:
            self.parser.feed(b"")
            return None
        if not data:
            raise ConnectionError("preview connection closed")
        self.pending.extend(self.parser.feed(data))
        return self.pending.popleft() if self.pending else None

    def request(self, method, payload=None, timeout=3):
        self.seq += 1
        metadata = {"channel": "user.rpc", "type": "req", "method": method,
                    "seq": self.seq, "encoding": "json"}
        self.socket.settimeout(timeout)
        self.socket.sendall(encode_frame(metadata, json.dumps(payload or {}).encode()))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.receive(max(0.01, deadline - time.monotonic()))
            if frame and frame.metadata.get("type") == "rsp" and frame.metadata.get("seq") == self.seq:
                if frame.metadata.get("method") == method:
                    return frame.json()
        raise TimeoutError(f"no response to {method}")

    def subscribe(self):
        result = self.request("preview.subscribe")
        if result.get("ok") is not True:
            raise RuntimeError(f"preview subscription failed: {result}")
        return result


class Viewer:
    """Keep draining TCP while ffplay displays; retain only the latest JPEG."""

    def __init__(self):
        executable = shutil.which("ffplay")
        if not executable:
            raise RuntimeError("--view requires ffplay on PATH")
        self.process = subprocess.Popen(
            [executable, "-loglevel", "error", "-nostats", "-window_title", "ESP-VISION Preview", "-fflags", "nobuffer", "-flags", "low_delay",
             "-analyzeduration", "0", "-probesize", "32",
             "-f", "mjpeg", "-framerate", "30", "-use_wallclock_as_timestamps", "1", "-i", "pipe:0"], stdin=subprocess.PIPE)
        self.queue = queue.Queue(maxsize=1)
        self.worker = threading.Thread(target=self._write, daemon=True)
        self.worker.start()

    def _write(self):
        try:
            while True:
                jpeg = self.queue.get()
                if jpeg is None:
                    return
                self.process.stdin.write(jpeg)
                self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def show(self, jpeg):
        if self.process.poll() is not None:
            raise ConnectionError("preview window closed")
        try:
            self.queue.get_nowait()
        except queue.Empty:
            pass
        self.queue.put_nowait(jpeg)

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.show_stop()
        self.worker.join(timeout=2)
        try:
            self.process.stdin.close()
        except (BrokenPipeError, OSError):
            pass

    def show_stop(self):
        try:
            self.queue.get_nowait()
        except queue.Empty:
            pass
        self.queue.put_nowait(None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=3232)
    parser.add_argument("--view", action="store_true", help="display using ffplay")
    parser.add_argument("--duration", type=float, default=0, help="seconds; 0 waits until Ctrl-C")
    parser.add_argument("--save-last", type=Path, help="save the last received JPEG")
    args = parser.parse_args()
    observer = viewer = None
    count = 0
    last = None
    started = time.monotonic()
    try:
        if args.view:
            viewer = Viewer()
        observer = PreviewObserver(args.host, args.port)
        observer.subscribe()
        print("Subscribed to preview.frame; waiting for application img.flush()", file=sys.stderr)
        while not args.duration or time.monotonic() - started < args.duration:
            frame = observer.receive(0.25)
            if not frame or frame.metadata.get("channel") != "preview.frame":
                continue
            if frame.metadata.get("contentType") != "image/jpeg":
                continue
            count += 1
            last = frame
            if viewer:
                viewer.show(frame.payload)
    except KeyboardInterrupt:
        pass
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        if observer:
            observer.close()
        if viewer:
            viewer.close()
    if args.save_last and last:
        args.save_last.write_bytes(last.payload)
    report = {"frames": count, "badFrames": observer.parser.bad_frames,
              "last": last.metadata if last else None}
    print(json.dumps(report), file=sys.stderr)
    return 1 if observer.parser.bad_frames else 0


if __name__ == "__main__":
    sys.exit(main())
