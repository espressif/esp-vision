#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Camera and LCD checks for a flashed ESP-VISION board, over EV-MUX.

Runs in the hardware-in-the-loop CI job after tools/test_control_transport.py:
that test proves the transport contract, this one drives the camera end to end
- capture, framesizes, pixel formats, orientation, frame rate, host preview,
imlib on the captured frames, and the frames going out to the LCD - and logs
what it measured, so a job log says how the board behaved and not just whether
it answered.

Every check reports PASS, FAIL or SKIP; a board without a camera or a panel
reports SKIP rather than failing the job. The exit status is non-zero when any
check fails.

    tools/ci/device_checks.py --port /dev/ttyACM0 --output frame.jpg
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import sys
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

try:
    import serial
except ImportError as exc:  # pragma: no cover - host dependency guard
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc

FRAME_RE = re.compile(rb"\x1eEVMUX/1 h=(\d+) p=(\d+) c=([0-9A-Fa-f]{8})\r\n")
USER_RPC_METHODS = {"hello", "capabilities", "script.write", "script.run", "device.control"}
MARKER_OK = "EVCHK_OK"
MARKER_ERR = "EVCHK_ERR"
# Staged on the device by script.write; script.run then executes it.
SCRIPT_PATH = "/ev_device_check.py"
# script.write payload must fit EV_CONTROL_PAYLOAD_MAX (2048) once base64'd.
SCRIPT_CHUNK = 900
# Raised by the bindings when a board has no backend for the feature.
UNSUPPORTED = ("ESP_ERR_NOT_SUPPORTED", "ESP_ERR_NOT_FOUND", "no module named", "ENODEV")


class CheckFailure(Exception):
    """The device answered, but not the way the check requires."""


class CheckSkipped(Exception):
    """The feature does not exist on this board."""


@dataclass
class Frame:
    metadata: dict
    payload: bytes


@dataclass
class Result:
    name: str
    status: str
    detail: str
    seconds: float


class Link:
    """EV-MUX framing over one serial port, with REPL and preview bookkeeping."""

    def __init__(self, port: str, baudrate: int) -> None:
        self.port = port
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0.05, write_timeout=2)
        # Protocol v3: holding the port open asserts DTR, which routes both
        # streams here, so every response comes back on this port.
        self.serial.dtr = True
        self.serial.rts = False
        self.seq = 9000
        self.buffer = bytearray()
        self.pending: list[Frame] = []
        self.stdout = bytearray()
        self.preview_count = 0

    def close(self) -> None:
        try:
            self.serial.close()
        except (OSError, serial.SerialException):
            pass

    def next_seq(self) -> int:
        self.seq += 1
        return self.seq

    def build(self, channel: str, frame_type: str, method: Optional[str], payload: bytes, seq: int) -> bytes:
        metadata = {
            "sid": "ci-device-checks",
            "seq": seq,
            "channel": channel,
            "type": frame_type,
            "encoding": "json" if channel.endswith(".rpc") else "text",
        }
        if method is not None:
            metadata["method"] = method
        head = json.dumps(metadata, separators=(",", ":")).encode()
        header = f"\x1eEVMUX/1 h={len(head)} p={len(payload)} c=00000000\r\n".encode()
        return header + head + payload + b"\x1f"

    def take_frame(self) -> Optional[Frame]:
        match = FRAME_RE.search(self.buffer)
        if match is None:
            if len(self.buffer) > 8192:  # keep a tail long enough for a split header
                del self.buffer[:-64]
            return None
        head_len = int(match.group(1))
        payload_len = int(match.group(2))
        head_start = match.end()
        payload_start = head_start + head_len
        eof = payload_start + payload_len
        if len(self.buffer) < eof + 1:
            return None
        if self.buffer[eof:eof + 1] != b"\x1f":
            del self.buffer[:match.end()]  # bogus header, resync
            return self.take_frame()
        frame = Frame(
            metadata=json.loads(bytes(self.buffer[head_start:payload_start]).decode()),
            payload=bytes(self.buffer[payload_start:eof]),
        )
        del self.buffer[:eof + 1]
        return frame

    def pump(self) -> None:
        chunk = self.serial.read(16384)
        if chunk:
            self.buffer.extend(chunk)
        while True:
            frame = self.take_frame()
            if frame is None:
                break
            channel = frame.metadata.get("channel")
            if channel == "repl.stdout":
                self.stdout.extend(frame.payload)
            elif channel == "preview.frame":
                self.preview_count += 1
            else:
                self.pending.append(frame)
        if len(self.pending) > 200:
            del self.pending[:-200]

    def rpc(self, method: str, params: Optional[dict] = None, timeout: float = 10.0) -> Frame:
        channel = "user.rpc" if method in USER_RPC_METHODS else "debug.rpc"
        seq = self.next_seq()
        self.serial.write(self.build(channel, "req", method, json.dumps(params or {}).encode(), seq))
        deadline = time.monotonic() + timeout
        while True:
            for index, frame in enumerate(self.pending):
                if frame.metadata.get("seq") == seq and frame.metadata.get("type") == "rsp":
                    return self.pending.pop(index)
            if time.monotonic() >= deadline:
                raise CheckFailure(f"timed out waiting for the {method} response")
            self.pump()

    def rpc_json(self, method: str, params: Optional[dict] = None, timeout: float = 10.0) -> dict:
        payload = json.loads(self.rpc(method, params, timeout).payload.decode())
        if not payload.get("ok"):
            error = payload.get("error", {})
            code = error.get("code", "?")
            message = f"{code} {error.get('message', '')}".strip()
            if any(marker in message for marker in UNSUPPORTED):
                raise CheckSkipped(f"not available on this board: {message}")
            raise CheckFailure(f"{method} failed: {message}")
        return payload

    def interrupt(self) -> None:
        self.serial.write(self.build("repl.signal", "data", None, b"\x03", self.next_seq()))

    def upload(self, source: str) -> None:
        """Stage a script through script.write, chunked to stay inside a frame."""
        data = source.encode()
        for offset in range(0, len(data), SCRIPT_CHUNK):
            chunk = data[offset:offset + SCRIPT_CHUNK]
            self.rpc_json("script.write", {
                "path": SCRIPT_PATH,
                "encoding": "utf-8",
                "mode": "overwrite",
                "offset": offset,
                "totalBytes": len(data),
                "contentBase64": base64.b64encode(chunk).decode(),
            }, timeout=15)

    def run_on_device(self, body: str, timeout: float) -> str:
        """Run a snippet on the device and return its marker arguments.

        The snippet travels as a file (script.write + script.run), not as a
        REPL line: repl.stdin only buffers EV_CONTROL_REPL_RX_MAX (512) bytes,
        so anything longer would arrive truncated and fail to compile.
        """
        source = (
            "try:\n"
            + textwrap.indent(body.strip("\n"), "    ")
            + f"\nexcept Exception as exc:\n    print('{MARKER_ERR}', exc)\n"
        )
        self.interrupt()
        time.sleep(0.2)
        self.pump()
        self.stdout.clear()
        self.upload(source)
        self.rpc_json("script.run", {"path": SCRIPT_PATH}, timeout=15)

        deadline = time.monotonic() + timeout
        while True:
            self.pump()
            for line in self.stdout.decode("utf-8", "replace").splitlines():
                if line.startswith(MARKER_OK):
                    return line[len(MARKER_OK):].strip()
                if line.startswith(MARKER_ERR):
                    message = line[len(MARKER_ERR):].strip()
                    if any(marker.lower() in message.lower() for marker in UNSUPPORTED):
                        raise CheckSkipped(f"not available on this board: {message}")
                    raise CheckFailure(message)
            if time.monotonic() >= deadline:
                tail = self.stdout.decode("utf-8", "replace")[-160:].replace("\r\n", " ").strip()
                raise CheckFailure(f"no answer within {timeout:.0f}s; device said: {tail!r}")


def answers_hello(port: str, baudrate: int, timeout: float = 3.0) -> bool:
    """Report whether a board on this port answers the EV-MUX hello RPC."""
    try:
        link = Link(port, baudrate)
    except (OSError, serial.SerialException):
        return False
    try:
        link.rpc_json("hello", timeout=timeout)
        return True
    except (CheckFailure, CheckSkipped, ValueError, OSError, serial.SerialException):
        return False
    finally:
        link.close()


@dataclass
class Context:
    link: Link
    args: argparse.Namespace
    notes: list[str] = field(default_factory=list)

    def note(self, text: str) -> None:
        """Record an extra log line printed underneath the check result."""
        self.notes.append(text)

    def script(self, *parts: str) -> str:
        """Join snippet parts, dedenting each one separately.

        A helper returning flush-left lines and an indented f-string literal
        have different leading whitespace; dedenting only after concatenation
        would keep the literal's indentation and the device would refuse the
        file with IndentationError.
        """
        return "\n".join(textwrap.dedent(part).strip("\n") for part in parts if part.strip())

    def sensor_setup(self) -> str:
        return (
            "import sensor\n"
            "sensor.reset()\n"
            f"sensor.set_pixformat(sensor.{self.args.pixformat})\n"
            f"sensor.set_framesize(sensor.{self.args.framesize})\n"
            "sensor.skip_frames(n=3)\n"
        )


def check_identity(ctx: Context) -> str:
    hello = ctx.link.rpc_json("hello")
    firmware = hello.get("firmware", {})
    if firmware.get("id") != "esp-vision":
        raise CheckFailure(f"unexpected firmware identity: {hello!r}")
    capabilities = ctx.link.rpc_json("capabilities")
    state = ctx.link.rpc_json("transport.state")
    user = state.get("user", {})
    ctx.note(f"board={hello.get('board')} arch={hello.get('arch')} idf={hello.get('idf')}")
    ctx.note(f"features={', '.join(capabilities.get('features', []))}")
    ctx.note(f"user route={user.get('route')} ready={user.get('ready')} "
             f"debug route={state.get('debug', {}).get('route')}")
    return f"firmware {firmware.get('version')} on {hello.get('board')}, EV-MUX v{hello.get('evMuxVersion')}"


def check_memory(ctx: Context) -> str:
    memory = ctx.link.rpc_json("debug.info", {"scope": "memory"})
    heap = memory.get("heapFree", 0)
    if heap <= 0:
        raise CheckFailure(f"no internal heap left: {memory!r}")
    ctx.note(f"heap largest block {memory.get('heapLargest', 0) // 1024} KiB, "
             f"psram largest block {memory.get('psramLargest', 0) // 1024} KiB")
    return f"heap free {heap // 1024} KiB, psram free {memory.get('psramFree', 0) // 1024} KiB"


def check_camera_capture(ctx: Context) -> str:
    """debug.capture_frame also brings the camera up if nothing did yet."""
    frame = ctx.link.rpc("debug.capture_frame", {"quality": ctx.args.quality}, timeout=ctx.args.frame_timeout)
    if frame.metadata.get("encoding") != "binary":
        payload = json.loads(frame.payload.decode())
        error = payload.get("error", {})
        message = f"{error.get('code', '?')} {error.get('message', '')}".strip()
        if any(marker in message for marker in UNSUPPORTED):
            raise CheckSkipped(f"no camera backend: {message}")
        raise CheckFailure(f"capture failed: {message}")

    jpeg = frame.payload
    if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise CheckFailure(f"payload is not a complete JPEG ({len(jpeg)} bytes)")
    width, height = frame.metadata.get("width"), frame.metadata.get("height")
    if not width or not height:
        raise CheckFailure(f"capture metadata carries no frame size: {frame.metadata!r}")
    if ctx.args.output:
        path = Path(ctx.args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(jpeg)
        ctx.note(f"saved to {path}")

    sensor = ctx.link.rpc_json("debug.info", {"scope": "sensor"})
    if not sensor.get("ready"):
        raise CheckFailure(f"sensor reports not ready right after a capture: {sensor!r}")
    ctx.note(f"sensor id=0x{sensor.get('id', 0):04X} {sensor.get('width')}x{sensor.get('height')} "
             f"{sensor.get('pixformat')} hmirror={sensor.get('hmirror')} vflip={sensor.get('vflip')}")
    return f"{width}x{height} JPEG, {len(jpeg) / 1024:.1f} KiB at quality {ctx.args.quality}"


def check_framesizes(ctx: Context) -> str:
    """Walk the framesizes the sensor module exposes and capture one frame each."""
    answer = ctx.link.run_on_device(
        ctx.script(f"""
        import sensor
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        results = []
        for name in ('QQVGA', 'QVGA', 'VGA'):
            try:
                sensor.set_framesize(getattr(sensor, name))
                sensor.skip_frames(n=2)
                img = sensor.snapshot()
                results.append('%s=%dx%d/%dx%d' % (name, img.width(), img.height(),
                                                   sensor.width(), sensor.height()))
            except Exception as exc:
                results.append('%s=unsupported' % name)
        print('{MARKER_OK}', ' '.join(results))
        """),
        timeout=ctx.args.frame_timeout + 30,
    )
    working, broken = [], []
    for entry in answer.split():
        name, _, value = entry.partition("=")
        if value == "unsupported":
            ctx.note(f"{name}: not supported by this sensor")
            continue
        frame, _, reported = value.partition("/")
        if frame != reported:
            broken.append(f"{name}: frame is {frame} but sensor reports {reported}")
        else:
            working.append(f"{name} {frame}")
        ctx.note(f"{name}: captured {frame}, sensor reports {reported}")
    if broken:
        raise CheckFailure("; ".join(broken))
    if not working:
        raise CheckFailure("no framesize produced a frame")
    return f"captured at {', '.join(working)}"


def check_pixformats(ctx: Context) -> str:
    """Each pixel format must produce a frame whose buffer matches its depth."""
    answer = ctx.link.run_on_device(
        ctx.script(f"""
        import sensor
        sensor.reset()
        sensor.set_framesize(sensor.{ctx.args.framesize})
        results = []
        for name, depth in (('RGB565', 2), ('GRAYSCALE', 1)):
            try:
                sensor.set_pixformat(getattr(sensor, name))
                sensor.skip_frames(n=2)
                img = sensor.snapshot()
                results.append('%s=%dx%d:%d:%d' % (name, img.width(), img.height(), img.size(),
                                                   img.width() * img.height() * depth))
            except Exception as exc:
                results.append('%s=unsupported' % name)
        print('{MARKER_OK}', ' '.join(results))
        """),
        timeout=ctx.args.frame_timeout + 30,
    )
    working, broken = [], []
    for entry in answer.split():
        name, _, value = entry.partition("=")
        if value == "unsupported":
            ctx.note(f"{name}: not supported by this sensor")
            continue
        size, actual, expected = value.split(":")
        if actual != expected:
            broken.append(f"{name}: buffer is {actual} bytes, expected {expected}")
        else:
            working.append(name)
        ctx.note(f"{name}: {size}, {int(actual) / 1024:.1f} KiB buffer")
    if broken:
        raise CheckFailure("; ".join(broken))
    if not working:
        raise CheckFailure("no pixel format produced a frame")
    return f"{', '.join(working)} frames have the expected buffer size"


def check_orientation(ctx: Context) -> str:
    """hmirror / vflip must stick and still deliver frames afterwards."""
    answer = ctx.link.run_on_device(
        ctx.script(ctx.sensor_setup(), f"""
        sensor.set_hmirror(True)
        sensor.set_vflip(True)
        sensor.skip_frames(n=2)
        img = sensor.snapshot()
        mirrored = (int(sensor.get_hmirror()), int(sensor.get_vflip()))
        sensor.set_hmirror(False)
        sensor.set_vflip(False)
        sensor.skip_frames(n=2)
        sensor.snapshot()
        print('{MARKER_OK}', mirrored[0], mirrored[1], int(sensor.get_hmirror()), int(sensor.get_vflip()),
              img.width(), img.height())
        """),
        timeout=ctx.args.frame_timeout + 20,
    )
    h_on, v_on, h_off, v_off, width, height = (int(value) for value in answer.split())
    if not (h_on and v_on):
        raise CheckFailure(f"hmirror/vflip did not turn on: hmirror={h_on} vflip={v_on}")
    if h_off or v_off:
        raise CheckFailure(f"hmirror/vflip did not turn off again: hmirror={h_off} vflip={v_off}")
    ctx.note(f"still capturing {width}x{height} while mirrored and flipped")
    return "hmirror and vflip toggle on and off, frames keep coming"


def check_snapshot_rate(ctx: Context) -> str:
    frames = ctx.args.frames
    answer = ctx.link.run_on_device(
        ctx.script(ctx.sensor_setup(), f"""
        import time
        img = sensor.snapshot()
        start = time.ticks_ms()
        for _ in range({frames}):
            img = sensor.snapshot()
        elapsed = time.ticks_diff(time.ticks_ms(), start)
        print('{MARKER_OK}', img.width(), img.height(), elapsed)
        """),
        timeout=ctx.args.frame_timeout + frames,
    )
    width, height, elapsed = (int(value) for value in answer.split())
    if elapsed <= 0:
        raise CheckFailure(f"implausible capture time for {frames} frames: {elapsed} ms")
    ctx.note(f"{elapsed / frames:.1f} ms per frame at {width}x{height} {ctx.args.pixformat}")
    return f"{frames} frames in {elapsed} ms ({1000.0 * frames / elapsed:.1f} fps)"


def check_preview_stream(ctx: Context) -> str:
    frames = ctx.args.frames
    ctx.link.preview_count = 0
    ctx.link.run_on_device(
        ctx.script(ctx.sensor_setup(), f"""
        for _ in range({frames}):
            sensor.snapshot().flush()
        print('{MARKER_OK}', {frames})
        """),
        timeout=ctx.args.frame_timeout + frames,
    )
    deadline = time.monotonic() + 2.0  # let frames still in flight arrive
    while time.monotonic() < deadline:
        ctx.link.pump()
    received = ctx.link.preview_count
    if received == 0:
        raise CheckFailure(f"no preview.frame arrived for {frames} flushed frames")
    return f"{received} preview.frame received for {frames} flushes"


def check_image_ops(ctx: Context) -> str:
    answer = ctx.link.run_on_device(
        ctx.script(ctx.sensor_setup(), f"""
        img = sensor.snapshot()
        width, height = img.width(), img.height()
        img.draw_rectangle(4, 4, 40, 30, color=(255, 0, 0), thickness=2)
        img.draw_line(0, 0, width - 1, height - 1, color=(0, 255, 0), thickness=2)
        img.draw_string(4, 40, 'CI', color=(255, 255, 255), scale=2)
        half = img.copy(x_scale=0.5, y_scale=0.5)
        mean = img.get_statistics().mean()
        jpeg = img.compress(quality=60)
        print('{MARKER_OK}', width, height, half.width(), half.height(), mean, jpeg.size())
        """),
        timeout=ctx.args.frame_timeout + 20,
    )
    width, height, half_w, half_h, mean, jpeg_size = (int(value) for value in answer.split())
    if half_w != width // 2 or half_h != height // 2:
        raise CheckFailure(f"half-scale copy is {half_w}x{half_h}, expected {width // 2}x{height // 2}")
    if jpeg_size <= 0:
        raise CheckFailure("compress() produced an empty JPEG")
    ctx.note(f"draw rectangle/line/string ok, copy {width}x{height} -> {half_w}x{half_h}, frame mean {mean}")
    return f"imlib ops on {width}x{height}, compressed to {jpeg_size / 1024:.1f} KiB"


def check_display(ctx: Context) -> str:
    """Panel bring-up: a color-bar pattern plus backlight control."""
    answer = ctx.link.run_on_device(
        ctx.script(f"""
        import display, image
        lcd = display.Display(backlight=100)
        try:
            frame = image.Image(lcd.width(), lcd.height(), image.RGB565)
            colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
            bar = frame.width() // len(colors)
            for index, color in enumerate(colors):
                frame.draw_rectangle(index * bar, 0, bar, frame.height(), color=color, fill=True)
            frame.draw_string(8, 8, 'ESP-VISION CI', color=(0, 0, 0), scale=2)
            lcd.write(frame, fit=False)
            lcd.backlight(40)
            dimmed = lcd.backlight()
            lcd.backlight(100)
            print('{MARKER_OK}', lcd.width(), lcd.height(), dimmed, lcd.backlight())
        finally:
            lcd.deinit()
        """),
        timeout=30,
    )
    width, height, dimmed, restored = (int(value) for value in answer.split())
    if dimmed != 40 or restored != 100:
        raise CheckFailure(f"backlight did not follow the requested levels: got {dimmed} then {restored}")
    ctx.note(f"backlight stepped to {dimmed}% and back to {restored}%")
    return f"{width}x{height} color bars written to the panel"


def check_display_stream(ctx: Context) -> str:
    """The path this board is built for: camera frames straight onto the LCD."""
    frames = max(10, ctx.args.frames // 2)
    answer = ctx.link.run_on_device(
        ctx.script(ctx.sensor_setup(), f"""
        import display, time
        lcd = display.Display(backlight=100)
        try:
            img = sensor.snapshot()
            lcd.write(img)
            start = time.ticks_ms()
            for _ in range({frames}):
                lcd.write(sensor.snapshot())
            elapsed = time.ticks_diff(time.ticks_ms(), start)
            lcd.clear()
            print('{MARKER_OK}', lcd.width(), lcd.height(), img.width(), img.height(), elapsed)
        finally:
            lcd.deinit()
        """),
        timeout=ctx.args.frame_timeout + frames * 2,
    )
    lcd_w, lcd_h, img_w, img_h, elapsed = (int(value) for value in answer.split())
    if elapsed <= 0:
        raise CheckFailure(f"implausible time for {frames} camera frames on the LCD: {elapsed} ms")
    ctx.note(f"{img_w}x{img_h} camera frames fitted onto a {lcd_w}x{lcd_h} panel, "
             f"{elapsed / frames:.1f} ms per frame")
    return f"{frames} camera frames drawn in {elapsed} ms ({1000.0 * frames / elapsed:.1f} fps)"


CHECKS: dict[str, tuple[Callable[[Context], str], str]] = {
    "identity": (check_identity, "firmware identity, capabilities and stream routing"),
    "memory": (check_memory, "free internal heap and PSRAM the camera draws from"),
    "camera": (check_camera_capture, "capture a JPEG and read back the sensor state"),
    "framesizes": (check_framesizes, "capture one frame per supported framesize"),
    "pixformats": (check_pixformats, "capture one frame per supported pixel format"),
    "orientation": (check_orientation, "hmirror / vflip take effect and revert"),
    "snapshot_rate": (check_snapshot_rate, "sustained sensor.snapshot() frame rate"),
    "preview": (check_preview_stream, "img.flush() streams preview frames to the host"),
    "image_ops": (check_image_ops, "imlib drawing, scaling, statistics and JPEG compression"),
    "display": (check_display, "color-bar pattern and backlight control on the LCD"),
    "display_stream": (check_display_stream, "camera frames drawn straight onto the LCD"),
}


def run(args: argparse.Namespace) -> int:
    link = Link(args.port, args.baudrate)
    ctx = Context(link=link, args=args)
    selected = list(CHECKS) if args.checks == "all" else [name.strip() for name in args.checks.split(",")]
    for name in selected:
        if name not in CHECKS:
            raise SystemExit(f"unknown check {name!r}; available: {', '.join(CHECKS)}")

    print(f"device checks on {args.port} ({len(selected)} checks, "
          f"{args.framesize}/{args.pixformat}, {args.frames} frames per timing check)")
    results: list[Result] = []
    try:
        for index, name in enumerate(selected, start=1):
            function, description = CHECKS[name]
            print(f"[{index:2d}/{len(selected)}] {name:<16} {description}", flush=True)
            ctx.notes.clear()
            started = time.monotonic()
            try:
                detail, status = function(ctx), "PASS"
            except CheckSkipped as exc:
                detail, status = str(exc), "SKIP"
            except CheckFailure as exc:
                detail, status = str(exc), "FAIL"
            seconds = time.monotonic() - started
            print(f"          {status} in {seconds:5.1f}s  {detail}")
            for note in ctx.notes:
                print(f"            - {note}")
            results.append(Result(name, status, detail, seconds))
    finally:
        try:  # drop the staged script and leave the board at a quiet REPL
            link.interrupt()
            time.sleep(0.2)
            cleanup = f"import os\r\ntry: os.remove('{SCRIPT_PATH}')\r\nexcept OSError: pass\r\n\r\n"
            link.serial.write(link.build("repl.stdin", "data", None, cleanup.encode(), link.next_seq()))
            time.sleep(0.3)
        except (OSError, serial.SerialException):
            pass
        link.close()

    failed = [item for item in results if item.status == "FAIL"]
    skipped = [item for item in results if item.status == "SKIP"]
    print("\ndevice check summary")
    for item in results:
        print(f"  {item.status:<4} {item.name:<16} {item.seconds:5.1f}s  {item.detail}")
    print(f"  {len(results) - len(failed) - len(skipped)} passed, {len(failed)} failed, {len(skipped)} skipped, "
          f"{sum(item.seconds for item in results):.1f}s total")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="serial port the board answers EV-MUX on")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--checks", default="all", help="all, or a comma-separated list: " + ", ".join(CHECKS))
    parser.add_argument("--output", help="write the captured JPEG here")
    parser.add_argument("--frames", type=int, default=30, help="frames used by the timing checks")
    parser.add_argument("--framesize", default="QVGA", help="sensor framesize constant")
    parser.add_argument("--pixformat", default="RGB565", help="sensor pixformat constant")
    parser.add_argument("--quality", type=int, default=60, help="JPEG quality for debug.capture_frame")
    parser.add_argument("--frame-timeout", type=float, default=25.0, help="seconds to wait for a camera frame")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, serial.SerialException) as exc:
        print(f"FAIL: serial error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
