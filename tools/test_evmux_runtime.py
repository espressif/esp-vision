#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Validate EV-MUX protocol v3 (two-stream user/debug, DTR routing) on real hardware.

The active sink is decided by DTR alone — holding the USB-OTG port open
routes both streams (user and debug) to CDC; closing it falls everything
back to USJ. No work.activate, no lease, no heartbeat.

- Phase A: the user.rpc plane (hello / debug.rpc transport.state) stays
  reachable on CDC while the device runs a blocking user script, and preview
  frames keep flowing on the user stream.
- Phase B: a framed ``repl.signal`` (payload 0x03) stops the running script
  with KeyboardInterrupt, without NLR failure or soft reset.
- Phase C: on an idle device, closing the CDC port falls the user stream
  back to USJ (cdc_disconnected); reopening returns it to CDC
  (cdc_connected).
- Phase D: the same close/reopen cycle while a preview loop runs — preview
  keeps flowing on USJ after fallback and moves back to CDC on reopen.
- Phase E: debug.rpc is accepted and answered on the active sink; a response
  never leaks to the other sink; debug.rpc on the non-active sink is rejected.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

try:
    import serial
except ImportError as exc:  # pragma: no cover - host dependency guard
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc


PROMPT = b">>> "
FRAME_RE = re.compile(rb"\x1eEVMUX/1 h=(\d+) p=(\d+) c=([0-9A-Fa-f]{8})\r\n")
LOOP_CODE = (
    'exec("import sensor\\n'
    'while True:\\n'
    ' img=sensor.snapshot()\\n'
    ' img.flush()")\r\n'
)


@dataclass
class EvMuxFrame:
    metadata: dict
    payload: bytes
    frame_start: int
    frame_end: int


class Capture:
    """Incremental EV-MUX frame reader: raw bytes are consumed into frames so
    a continuous preview flood does not make parsing quadratic."""

    def __init__(self, name: str, port: str, baudrate: int) -> None:
        self.name = name
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0.02, write_timeout=1)
        self.serial.dtr = True
        self.serial.rts = False
        self.data = bytearray()
        self.lock = threading.Lock()
        self.offset = 0

    def append(self, chunk: bytes) -> None:
        with self.lock:
            self.data.extend(chunk)

    def snapshot(self) -> bytes:
        with self.lock:
            return bytes(self.data)

    def clear(self) -> None:
        with self.lock:
            self.data.clear()
            self.offset = 0

    def discard(self, end: int) -> None:
        with self.lock:
            del self.data[:end]
            self.offset = 0

    def close(self) -> None:
        self.serial.close()


def reader(capture: Capture, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            chunk = capture.serial.read(8192)
        except (serial.SerialException, TypeError, OSError):
            # Port closed underneath us (teardown / reconnect tests).
            return
        if chunk:
            capture.append(chunk)
        else:
            time.sleep(0.01)


def parse_first_evmux(data: bytes, allow_partial: bool = False) -> Optional[EvMuxFrame]:
    match = FRAME_RE.search(data)
    if not match:
        return None

    metadata_len = int(match.group(1))
    payload_len = int(match.group(2))
    metadata_start = match.end()
    payload_start = metadata_start + metadata_len
    eof_pos = payload_start + payload_len
    frame_end = eof_pos + 1
    if len(data) < frame_end:
        return None
    if data[eof_pos:eof_pos + 1] != b"\x1f":
        # Corrupt or misaligned candidate; skip it so the caller resyncs.
        return None

    metadata = json.loads(data[metadata_start:payload_start].decode("utf-8"))
    return EvMuxFrame(metadata, data[payload_start:eof_pos], match.start(), frame_end)


def new_frames(capture: Capture) -> list[EvMuxFrame]:
    """Return frames completed since the last call (O(new data) only)."""
    data = capture.snapshot()
    frames: list[EvMuxFrame] = []
    offset = capture.offset
    while True:
        frame = parse_first_evmux(data[offset:], allow_partial=True)
        if frame is None:
            if not FRAME_RE.search(data[offset:]):
                # No header candidate at all: everything is garbage/resync.
                offset = len(data)
            break
        frames.append(frame)
        offset += frame.frame_end
    capture.offset = offset
    if capture.offset > 1 * 1024 * 1024:
        capture.discard(capture.offset)
    return frames


def make_evmux(channel: str, frame_type: str, method: Optional[str], payload: bytes, seq: int) -> bytes:
    metadata = {
        "sid": "host-p0-runtime-test",
        "seq": seq,
        "channel": channel,
        "type": frame_type,
        "encoding": "json" if channel.endswith(".rpc") else "text",
    }
    if method is not None:
        metadata["method"] = method
    metadata_bytes = json.dumps(metadata, separators=(",", ":")).encode("utf-8")
    header = f"\x1eEVMUX/1 h={len(metadata_bytes)} p={len(payload)} c=00000000\r\n".encode("ascii")
    return header + metadata_bytes + payload + b"\x1f"


class Host:
    def __init__(self, usj: Capture, cdc: Capture) -> None:
        self.usj = usj
        self.cdc = cdc
        self.seq = 100

    def next_seq(self) -> int:
        self.seq += 1
        return self.seq

    def send_rpc(self, capture: Capture, method: str, payload: dict) -> None:
        capture.serial.write(make_evmux("user.rpc", "req", method, json.dumps(payload).encode("utf-8"), self.next_seq()))

    def send_repl(self, capture: Capture, text: str) -> None:
        capture.serial.write(make_evmux("repl.stdin", "data", None, text.encode("utf-8"), self.next_seq()))

    def send_signal_ctrl_c(self, capture: Capture) -> None:
        capture.serial.write(make_evmux("repl.signal", "data", None, b"\x03", self.next_seq()))

    def wait_frame(
        self,
        captures: list[Capture],
        predicate: Callable[[Capture, EvMuxFrame], bool],
        timeout: float,
        what: str,
    ) -> tuple[Capture, EvMuxFrame]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for capture in captures:
                for frame in new_frames(capture):
                    if predicate(capture, frame):
                        return capture, frame
            time.sleep(0.02)
        raise TimeoutError(f"timed out waiting for {what}")

    def expect_no_frame(
        self,
        capture: Capture,
        predicate: Callable[[EvMuxFrame], bool],
        timeout: float,
        what: str,
    ) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for frame in new_frames(capture):
                if predicate(frame):
                    raise AssertionError(f"unexpected frame on {capture.name} ({what}): {frame.metadata!r}")
            time.sleep(0.02)

    def wait_channel_text(self, capture: Capture, channel: str, marker: bytes, timeout: float, what: str) -> bytes:
        deadline = time.monotonic() + timeout
        text = bytearray()
        while time.monotonic() < deadline:
            for frame in new_frames(capture):
                if frame.metadata.get("channel") == channel:
                    text.extend(frame.payload)
                    if marker in text:
                        return bytes(text)
            time.sleep(0.02)
        raise TimeoutError(f"{capture.name}: timed out waiting for {what}; got {text[-300:]!r}")

    def wait_raw_text(self, capture: Capture, marker: bytes, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in capture.snapshot():
                return capture.snapshot()
            time.sleep(0.02)
        raise TimeoutError(f"{capture.name}: timed out waiting for {marker!r}")


def rpc_payload(frame: EvMuxFrame) -> dict:
    payload = json.loads(frame.payload.decode("utf-8"))
    if not payload.get("ok"):
        raise AssertionError(f"RPC failed: metadata={frame.metadata!r} payload={payload!r}")
    return payload


def is_method(method: str) -> Callable[..., bool]:
    # Usable both as wait_frame predicate (cap, frame) and expect_no_frame
    # predicate (frame).
    return lambda *args: args[-1].metadata.get("method") == method


def ensure_evmux(host: Host, cdc: Capture) -> None:
    # Protocol v3: holding the CDC port open (DTR) makes it the active sink —
    # no work.activate, no lease. Prove the protocol is up and the user
    # stream route followed DTR onto this port, then prove the REPL is live.
    cdc.clear()
    host.send_rpc(cdc, "hello", {})
    host.wait_frame([cdc], is_method("hello"), 5, "framed hello")
    wait_control_route(host, cdc, "cdc", 5)
    host.send_signal_ctrl_c(cdc)
    host.send_repl(cdc, 'print("EVBOOT_OK")\r\n')
    host.wait_channel_text(cdc, "repl.stdout", b"EVBOOT_OK", 8, "REPL live marker")
    cdc.clear()


def wait_control_route(host: Host, capture: Capture, route: str, timeout: float) -> None:
    host.send_rpc(capture, "device.control", {"action": "status"})
    _, frame = host.wait_frame([capture], is_method("device.control"), timeout, "device.control status rsp")
    payload = rpc_payload(frame)
    if payload.get("user", {}).get("route") != route:
        raise AssertionError(f"user route is {payload.get('user')!r}, expected {route!r}")


def start_loop(host: Host, cdc: Capture) -> None:
    host.send_repl(cdc, LOOP_CODE)
    # The loop is running when preview frames start flowing on CDC.
    host.wait_frame(
        [cdc],
        lambda cap, frame: frame.metadata.get("channel") == "preview.frame",
        8,
        "preview.frame from the running loop",
    )


def stop_loop(host: Host, capture: Capture) -> None:
    # Protocol v3: the route stays on this port for as long as it is held
    # open, so the signal can be sent directly.
    host.send_signal_ctrl_c(capture)
    # MicroPython prints uncaught exception tracebacks to stdout.
    host.wait_channel_text(capture, "repl.stdout", b"KeyboardInterrupt", 8, "KeyboardInterrupt after repl.signal")
    print("    KeyboardInterrupt observed on repl.stdout: ok")


def assert_device_alive(host: Host) -> None:
    for capture in (host.usj, host.cdc):
        data = capture.snapshot()
        for marker in (b"NLR jump failed", b"MPY: soft reboot", b"Guru Meditation"):
            if marker in data:
                raise AssertionError(f"{capture.name}: device crashed: found {marker!r}")
    host.send_repl(host.cdc, 'print("EVMUX_ALIVE")\r\n')
    host.wait_channel_text(host.cdc, "repl.stdout", b"EVMUX_ALIVE", 8, "REPL still responsive")


def phase_a(host: Host) -> None:
    print("[A] control plane reachable while user script runs")
    start_loop(host, host.cdc)
    time.sleep(1.5)
    host.cdc.clear()

    host.send_rpc(host.cdc, "hello", {})
    host.wait_frame([host.cdc], is_method("hello"), 5, "hello rsp during running script")
    print("    hello answered during running script: ok")

    host.cdc.serial.write(make_evmux("debug.rpc", "req", "transport.state", b"{}", host.next_seq()))
    _, frame = host.wait_frame(
        [host.cdc],
        lambda cap, item: item.metadata.get("channel") == "debug.rpc"
        and item.metadata.get("type") == "rsp"
        and item.metadata.get("method") == "transport.state",
        5,
        "transport.state rsp during running script",
    )
    payload = rpc_payload(frame)
    if payload.get("user", {}).get("route") != "cdc":
        raise AssertionError(f"user route drifted: {payload!r}")
    print("    transport.state answered on cdc, user route stable: ok")


def close_cdc(host: Host, what: str) -> None:
    # Dropping the port pulls DTR low: every stream must fall back to USJ
    # within one transport poll period — no lease, no timeout.
    host.cdc.close()
    host.wait_frame(
        [host.usj],
        lambda cap, item: item.metadata.get("method") == "route.changed"
        and json.loads(item.payload.decode("utf-8")).get("stream") == "user"
        and json.loads(item.payload.decode("utf-8")).get("route") == "usj",
        6,
        what,
    )


def reopen_cdc(host: Host, args: argparse.Namespace, stop: threading.Event) -> None:
    cdc = Capture("cdc", args.cdc, args.baudrate)
    threading.Thread(target=reader, args=(cdc, stop), daemon=True).start()
    host.cdc = cdc
    host.wait_frame(
        [host.cdc],
        lambda cap, item: item.metadata.get("method") == "route.changed"
        and json.loads(item.payload.decode("utf-8")).get("stream") == "user"
        and json.loads(item.payload.decode("utf-8")).get("route") == "cdc",
        8,
        "route.changed(user->cdc, cdc_connected) after reopen",
    )


def phase_c(host: Host, args: argparse.Namespace, stop: threading.Event) -> None:
    print("[C] idle: closing CDC falls back to USJ; reopening returns to CDC")
    close_cdc(host, "route.changed(user->usj) on idle device after CDC close")
    print("    route.changed(user->usj, cdc_disconnected): ok")
    wait_control_route(host, host.usj, "usj", 5)
    print("    user route settled on usj: ok")

    reopen_cdc(host, args, stop)
    print("    route.changed(user->cdc, cdc_connected): ok")
    wait_control_route(host, host.cdc, "cdc", 5)
    print("    user route settled back on cdc: ok")


def phase_d(host: Host, args: argparse.Namespace, stop: threading.Event) -> None:
    print("[D] running loop survives CDC close; route and preview recover on reopen")
    wait_control_route(host, host.cdc, "cdc", 5)
    start_loop(host, host.cdc)
    host.usj.clear()
    close_cdc(host, "route.changed(user->usj) while script runs")
    print("    user stream fell back to usj while the loop runs: ok")

    # The running loop keeps flushing preview; the stream follows DTR to USJ.
    host.wait_frame(
        [host.usj],
        lambda cap, item: item.metadata.get("channel") == "preview.frame",
        8,
        "preview.frame on usj after fallback",
    )
    print("    preview kept flowing on usj: ok")

    reopen_cdc(host, args, stop)
    print("    route switched back to cdc on reopen: ok")
    host.wait_frame(
        [host.cdc],
        lambda cap, item: item.metadata.get("channel") == "preview.frame",
        8,
        "preview.frame back on cdc after reopen",
    )
    print("    preview moved back to cdc: ok")

    stop_loop(host, host.cdc)
    assert_device_alive(host)
    print("    loop stopped after recovery; device healthy: ok")


def phase_b(host: Host) -> None:
    print("[B] framed repl.signal (0x03) stops the running script cleanly")
    stop_loop(host, host.cdc)
    assert_device_alive(host)
    print("    no NLR failure / soft reboot; REPL responsive: ok")


def phase_e(host: Host) -> None:
    print("[E] debug.rpc accepted on the active sink and answered there")
    wait_control_route(host, host.cdc, "cdc", 5)
    host.usj.clear()
    host.cdc.clear()
    # With CDC held open it is the single active sink: debug.rpc is accepted
    # here and the response returns here (not via the USJ debug route).
    host.cdc.serial.write(make_evmux("debug.rpc", "req", "transport.state", b"{}", host.next_seq()))
    _, frame = host.wait_frame(
        [host.cdc],
        lambda cap, item: item.metadata.get("channel") == "debug.rpc"
        and item.metadata.get("type") == "rsp"
        and item.metadata.get("method") == "transport.state",
        5,
        "transport.state rsp on CDC",
    )
    payload = rpc_payload(frame)
    if payload.get("sinks", {}).get("usj", {}).get("state") != "active":
        raise AssertionError(f"unexpected transport.state payload: {payload!r}")
    print("    transport.state answered on cdc: ok")

    # The response must not be duplicated/leaked onto USJ.
    host.expect_no_frame(
        host.usj,
        lambda item: item.metadata.get("channel") == "debug.rpc"
        and item.metadata.get("method") == "transport.state",
        2,
        "debug.rpc response must not leak to USJ",
    )
    print("    no debug.rpc response leaked to usj: ok")

    # While CDC is the active sink, debug.rpc arriving on USJ is rejected:
    # only the active sink carries RPC traffic (protocol v3).
    host.usj.clear()
    host.usj.serial.write(make_evmux("debug.rpc", "req", "transport.state", b"{}", host.next_seq()))
    host.expect_no_frame(
        host.usj,
        lambda item: item.metadata.get("channel") == "debug.rpc"
        and item.metadata.get("type") == "rsp"
        and item.metadata.get("method") == "transport.state",
        3,
        "debug.rpc on the non-active sink must be rejected",
    )
    print("    debug.rpc on non-active usj rejected: ok")


def run(args: argparse.Namespace) -> None:
    usj = Capture("usj", args.usj, args.baudrate)
    cdc = Capture("cdc", args.cdc, args.baudrate)
    stop = threading.Event()
    threads = [
        threading.Thread(target=reader, args=(usj, stop), daemon=True),
        threading.Thread(target=reader, args=(cdc, stop), daemon=True),
    ]
    for thread in threads:
        thread.start()

    host = Host(usj, cdc)
    try:
        ensure_evmux(host, cdc)
        print("[0] EV-MUX bootstrap + DTR route to cdc: ok")

        phase_a(host)
        phase_b(host)
        phase_c(host, args, stop)
        phase_d(host, args, stop)
        phase_e(host)
    finally:
        # Best-effort cleanup: leave the device at a quiet REPL.
        try:
            host.send_signal_ctrl_c(host.cdc)
            time.sleep(0.5)
        except Exception:
            pass
        stop.set()
        for thread in threads:
            thread.join(timeout=1)
        usj.close()
        cdc.close()

    print("PASS: all EV-MUX runtime checks passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--usj", default="/dev/ttyACM1", help="USB-Serial-JTAG port")
    parser.add_argument("--cdc", default="/dev/ttyACM0", help="USB-OTG CDC port")
    parser.add_argument("--baudrate", type=int, default=115200)
    args = parser.parse_args()
    try:
        run(args)
    except (AssertionError, TimeoutError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
