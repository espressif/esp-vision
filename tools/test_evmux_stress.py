#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""EV-MUX protocol v3 transport stress matrix.

- S1: frame fragmentation (1/2/7/64/128-byte writes) reassembles correctly.
- S2: payload bytes 0x03/0x1e/0x1f pass through the parser intact.
- S3: USJ+CDC dual ingress independence; parser resyncs after garbage.
- S4: 10x CDC close/reopen cycles; each toggle emits route.changed x2
  (cdc_disconnected / cdc_connected) with no protocol residue.
- S5: 60s preview flood with the route pinned by DTR (no keepalive in v3),
  then transport.stats counters.
- S6: long C-call (imlib) loop keeps answering host RPCs.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - host dependency guard
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from test_evmux_runtime import (  # noqa: E402
    Capture,
    Host,
    ensure_evmux,
    is_method,
    make_evmux,
    new_frames,
    reader,
    rpc_payload,
    start_loop,
    stop_loop,
    wait_control_route,
)

SPECIAL_PAYLOAD = b'x=b"\x1e\x1f"\r\nprint("SPECLEN:",len(x))\r\n'
CTRL_C_PAYLOAD = b'print("SA")\r\n\x03print("SB")\r\n'


def cdc_reader(capture: Capture, stop: threading.Event) -> None:
    # Same loop as the p0 reader, but exits quietly when the port is closed
    # underneath it -- S4 drops the CDC port on purpose ten times.
    while not stop.is_set():
        try:
            chunk = capture.serial.read(8192)
        except (serial.SerialException, TypeError, OSError):
            return
        if chunk:
            capture.append(chunk)
        else:
            time.sleep(0.01)


def wait_channel_text(host: Host, capture: Capture, channel: str, marker: bytes, timeout: float, what: str) -> bytes:
    deadline = time.monotonic() + timeout
    text = bytearray()
    while time.monotonic() < deadline:
        for frame in new_frames(capture):
            if frame.metadata.get("channel") == channel:
                text.extend(frame.payload)
                if marker in text:
                    return bytes(text)
        time.sleep(0.02)
    raise TimeoutError(f"{capture.name}: timed out waiting for {what}; got {text[-200:]!r}")


def wait_route_changed_all(capture: Capture, route: str, reason: str, timeout: float, what: str) -> None:
    # Protocol v3: every route switch emits one route.changed event per stream
    # (user/debug) on the stream's new sink.
    remaining = {"user", "debug"}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for frame in new_frames(capture):
            if frame.metadata.get("method") == "route.changed":
                payload = json.loads(frame.payload.decode("utf-8"))
                if payload.get("route") == route and payload.get("reason") == reason:
                    remaining.discard(payload.get("stream"))
        if not remaining:
            return
        time.sleep(0.02)
    raise TimeoutError(f"{capture.name}: timed out waiting for {what}; missing {sorted(remaining)}")


def phase_s1(host: Host) -> None:
    print("[S1] fragmented frame writes reassemble")
    for chunk in (1, 2, 7, 64, 128):
        frame = make_evmux("user.rpc", "req", "hello", b"{}", host.next_seq())
        for i in range(0, len(frame), chunk):
            host.cdc.serial.write(frame[i:i + chunk])
            time.sleep(0.004)
        host.wait_frame([host.cdc], is_method("hello"), 6, f"hello fragmented at {chunk}B")
        print(f"    chunk={chunk}B: ok")


def phase_s2(host: Host) -> None:
    print("[S2] special payload bytes: transport passes, REPL input layer documented")
    # Baseline: escaped source (printable payload) round-trips fine.
    host.send_repl(host.cdc, 'x=b"\\x1e\\x1f"\r\nprint("SPECLEN:",len(x))\r\n')
    wait_channel_text(host, host.cdc, "repl.stdout", b"SPECLEN: 2", 6, "escaped payload round trip")
    print("    escaped 0x1e/0x1f source: ok (len=2)")

    # Raw 0x1e/0x1f in the frame payload: the length-prefixed parser must not
    # choke on the SOF byte (the second source line must still execute). The
    # REPL input layer legitimately drops non-printable editing bytes, so x
    # comes back empty — that is REPL semantics, not a transport loss.
    host.cdc.serial.write(make_evmux("repl.stdin", "data", None, b'x=b"\x1e\x1f"\r\nprint("RAWLEN:",len(x))\r\n', host.next_seq()))
    wait_channel_text(host, host.cdc, "repl.stdout", b"RAWLEN: 0", 6, "raw special bytes frame parses")
    print("    raw 0x1e/0x1f: frame parses intact (REPL filters input bytes): ok")

    # 0x03 in a payload must reach the REPL as an editing character (line
    # clear) without damaging the frame or anything around it.
    host.cdc.serial.write(make_evmux("repl.stdin", "data", None, CTRL_C_PAYLOAD, host.next_seq()))
    text = wait_channel_text(host, host.cdc, "repl.stdout", b"SB", 6, "0x03 payload editing semantics")
    if b"SA" not in text or b"SB" not in text:
        raise AssertionError(f"0x03 payload disturbed surrounding lines: {text!r}")
    print("    0x03 payload acts as REPL editing char, frame intact: ok")


def phase_s3(host: Host) -> None:
    print("[S3] dual ingress independence + resync")
    # Protocol v3: with the CDC port held open it is the only active sink.
    # hello/capabilities are still admitted on either USB sink (discovery);
    # everything else is accepted only from the active sink.
    host.usj.clear()
    host.cdc.clear()

    host.send_rpc(host.cdc, "hello", {})
    host.wait_frame([host.cdc], is_method("hello"), 5, "hello on active cdc")
    print("    hello answered on active cdc: ok")

    # Discovery on the non-active sink: a hello arriving on USJ is admitted,
    # but the response event is emitted on the user stream, which is CDC.
    host.send_rpc(host.usj, "hello", {})
    host.wait_frame([host.cdc], is_method("hello"), 5, "usj-ingress hello answered via user route (cdc)")
    print("    bootstrap hello admitted on usj, response routed to cdc: ok")

    # Non-bootstrap user.rpc on the non-active sink is rejected: no response
    # on USJ, and the rejection is logged on the debug stream (now on CDC).
    host.send_rpc(host.usj, "device.control", {"action": "status"})
    host.expect_no_frame(
        host.usj,
        lambda item: item.metadata.get("method") == "device.control",
        3,
        "device.control on non-active usj must be rejected",
    )
    wait_channel_text(host, host.cdc, "log.idf", b"reject ingress=usj", 5, "rejection log.idf record")
    print("    user.rpc on non-active usj rejected (logged via log.idf on cdc): ok")

    # Garbage between frames must not break subsequent parsing.
    host.cdc.serial.write(b"\xaa\xbb\xcc\x1eNOTEVMUX/1 garbage\r\n")
    host.send_rpc(host.cdc, "hello", {})
    host.wait_frame([host.cdc], is_method("hello"), 5, "hello after garbage")
    print("    parser resync after garbage: ok")


def send_debug_rpc(host: Host, method: str, payload: dict) -> None:
    # Protocol v3: debug.rpc is served on the active sink only; with the CDC
    # port held open that is CDC (the same frame on USJ would be rejected).
    host.cdc.serial.write(make_evmux("debug.rpc", "req", method, json.dumps(payload).encode("utf-8"), host.next_seq()))


def phase_s4(host: Host, args: argparse.Namespace, stop: threading.Event) -> None:
    print("[S4] 10x CDC reconnect cycles (DTR routing, no protocol residue)")
    for i in range(10):
        # Dropping the port pulls DTR low: both streams fall back to USJ.
        host.cdc.close()
        wait_route_changed_all(host.usj, "usj", "cdc_disconnected", 6, f"cycle {i} fallback events")

        # Reopening the port routes both streams back to CDC.
        cdc = Capture("cdc", args.cdc, args.baudrate)
        threading.Thread(target=cdc_reader, args=(cdc, stop), daemon=True).start()
        host.cdc = cdc
        wait_route_changed_all(host.cdc, "cdc", "cdc_connected", 8, f"cycle {i} reconnect events")

        # No residue: the RPC path answers immediately on the new route.
        host.send_rpc(host.cdc, "hello", {})
        host.wait_frame([host.cdc], is_method("hello"), 6, f"hello after cycle {i}")
        print(f"    cycle {i + 1}/10: cdc_disconnected x2 + cdc_connected x2 + hello: ok")
    wait_control_route(host, host.cdc, "cdc", 5)


def phase_s5(host: Host) -> None:
    print("[S5] 60s preview flood; route pinned by DTR (no keepalive in v3)")
    start_loop(host, host.cdc)
    host.cdc.clear()
    t0 = time.monotonic()
    frames = 0
    checks = 0
    while time.monotonic() - t0 < 60:
        time.sleep(1)
        frames += sum(1 for frame in new_frames(host.cdc) if frame.metadata.get("channel") == "preview.frame")
        elapsed = int(time.monotonic() - t0)
        if elapsed // 10 > checks:
            checks = elapsed // 10
            host.send_rpc(host.cdc, "device.control", {"action": "status"})
            _, frame = host.wait_frame([host.cdc], is_method("device.control"), 6, "status during flood")
            status = rpc_payload(frame)
            if status.get("user", {}).get("route") != "cdc":
                raise AssertionError(f"route drifted during flood: {status!r}")
            print(f"    t+{elapsed}s: route=cdc stable (no keepalive), preview_frames={frames}")
    if frames == 0:
        raise AssertionError("preview flood stalled: no preview.frame seen during 60s")
    print(f"    60s: preview_frames={frames}, route stayed on cdc without any keepalive")

    print("[S5b] transport.stats answers while the loop runs")
    send_debug_rpc(host, "debug.info", {"scope": "transport.stats"})
    _, frame = host.wait_frame([host.cdc], is_method("debug.info"), 6, "transport.stats")
    stats = rpc_payload(frame)
    print(f"    stats: rx={stats.get('rx')} frames={stats.get('frames')} "
          f"ringFull={stats.get('ringFull')} drops={stats.get('drops')}")
    if stats.get("frames", {}).get("ok", 0) == 0:
        raise AssertionError(f"transport.stats counters look dead: {stats!r}")
    drops = stats.get("drops", {})
    for counter in ("replRx", "vmBusy", "txTimeout", "txDrop"):
        if counter not in drops:
            raise AssertionError(f"transport.stats drops missing {counter}: {stats!r}")

    stop_loop(host, host.cdc)


def phase_s6(host: Host) -> None:
    print("[S6] long C-call loop keeps answering host RPCs")
    code = (
        'exec("import sensor,time\\n'
        't=time.ticks_ms()\\n'
        'while time.ticks_diff(time.ticks_ms(),t)<12000:\\n'
        ' img=sensor.snapshot()\\n'
        ' img.histeq()\\n'
        ' img.lens_corr(1.8)")\r\n'
    )
    host.send_repl(host.cdc, code)
    time.sleep(2)  # the loop is deep in imlib C calls now
    for i in range(3):
        host.send_rpc(host.cdc, "hello", {})
        host.wait_frame([host.cdc], is_method("hello"), 6, f"hello during long C {i}")
        host.send_rpc(host.cdc, "device.control", {"action": "status"})
        _, frame = host.wait_frame([host.cdc], is_method("device.control"), 6, f"status during long C {i}")
        status = rpc_payload(frame)
        if status.get("user", {}).get("route") != "cdc":
            raise AssertionError(f"route drifted during long C call: {status!r}")
    print("    3x hello+status during histeq/lens_corr loop: ok")
    # The loop self-terminates after ~12s. Protocol v3 has no lease to renew:
    # the route stays on CDC for as long as the port is held open.
    time.sleep(12)
    wait_control_route(host, host.cdc, "cdc", 5)
    host.send_repl(host.cdc, 'print("AFTER_C_OK")\r\n')
    wait_channel_text(host, host.cdc, "repl.stdout", b"AFTER_C_OK", 8, "REPL after long C")


def run(args: argparse.Namespace) -> None:
    usj = Capture("usj", args.usj, args.baudrate)
    cdc = Capture("cdc", args.cdc, args.baudrate)
    stop = threading.Event()
    threads = [
        threading.Thread(target=reader, args=(usj, stop), daemon=True),
        threading.Thread(target=cdc_reader, args=(cdc, stop), daemon=True),
    ]
    for thread in threads:
        thread.start()

    host = Host(usj, cdc)
    try:
        ensure_evmux(host, cdc)
        print("[0] EV-MUX bootstrap + REPL live: ok")
        phase_s1(host)
        phase_s2(host)
        phase_s3(host)
        phase_s4(host, args, stop)
        phase_s5(host)
        phase_s6(host)
    finally:
        try:
            host.send_signal_ctrl_c(host.cdc)
            time.sleep(0.5)
        except Exception:
            pass
        stop.set()
        for thread in threads:
            thread.join(timeout=1)
        usj.close()
        # host.cdc tracks the latest reopened capture (S4); close() is
        # idempotent if a failed cycle already dropped the port.
        host.cdc.close()

    print("PASS: all stress checks passed")


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
