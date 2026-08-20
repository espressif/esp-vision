#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Validate ESP-VISION EV-MUX user.rpc / debug.rpc commands.

Protocol v3 (DTR routing): the port this test holds open is the active sink,
so every RPC (user.rpc and debug.rpc) is answered on it. The lease model
(transport.activate/release, work.*, heartbeat) is hard-deleted — calling
those methods returns UNKNOWN_METHOD, and the route follows DTR instead.
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

try:
    import serial
except ImportError as exc:  # pragma: no cover - host dependency guard
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc


PROMPT = b">>> "
FRAME_RE = re.compile(rb"\x1eEVMUX/1 h=(\d+) p=(\d+) c=([0-9A-Fa-f]{8})\r\n")
USER_RPC_METHODS = {
    "hello",
    "capabilities",
    "script.write",
    "script.run",
    "device.control",
}
# Removed in EV-MUX contract v2; kept in the routing table so the
# UNKNOWN_METHOD checks still exercise the user.rpc domain.
REMOVED_USER_RPC_METHODS = {
    "work.activate",
    "work.heartbeat",
    "work.release",
}


def rpc_channel_for_method(method: str) -> str:
    return "user.rpc" if method in USER_RPC_METHODS or method in REMOVED_USER_RPC_METHODS else "debug.rpc"


@dataclass
class EvMuxFrame:
    metadata: dict
    payload: bytes
    frame_start: int
    frame_end: int


class Transport:
    def __init__(self, port: str, baudrate: int) -> None:
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0.05, write_timeout=1)
        # Protocol v3: holding the port open asserts DTR, which alone decides
        # the active sink (USB-OTG only; harmless on USJ).
        self.serial.dtr = True
        self.serial.rts = False
        self.seq = 200
        self.frame_buffer = bytearray()
        self.pending_frames: list[EvMuxFrame] = []

    def close(self) -> None:
        self.serial.close()

    def frame(self, channel: str, frame_type: str, method: Optional[str], payload: bytes) -> bytes:
        metadata = {
            "sid": "host-p2-test",
            "seq": self.seq,
            "channel": channel,
            "type": frame_type,
            "encoding": "json" if channel.endswith(".rpc") else "text",
        }
        self.seq += 1
        if method is not None:
            metadata["method"] = method
        metadata_bytes = json.dumps(metadata, separators=(",", ":")).encode("utf-8")
        header = f"\x1eEVMUX/1 h={len(metadata_bytes)} p={len(payload)} c=00000000\r\n".encode("ascii")
        return header + metadata_bytes + payload + b"\x1f"

    def send_rpc(self, method: str, payload: dict) -> None:
        self.serial.write(self.frame(rpc_channel_for_method(method), "req", method, json.dumps(payload).encode("utf-8")))

    def send_repl(self, text: str) -> None:
        self.serial.write(self.frame("repl.stdin", "data", None, text.encode("utf-8")))

    def pop_frame(self) -> Optional[EvMuxFrame]:
        frame = parse_first_evmux(bytes(self.frame_buffer), allow_partial=True)
        if frame is None:
            if len(self.frame_buffer) > 8192:
                marker = self.frame_buffer.rfind(b"\x1eEVMUX/1")
                if marker > 0:
                    del self.frame_buffer[:marker]
                elif marker < 0:
                    del self.frame_buffer[:-1024]
            return None
        del self.frame_buffer[:frame.frame_end]
        return frame


def parse_first_evmux(data: bytes, allow_partial: bool = False) -> Optional[EvMuxFrame]:
    match = FRAME_RE.search(data)
    if not match:
        if allow_partial:
            return None
        raise AssertionError("missing EV-MUX frame header")

    metadata_len = int(match.group(1))
    payload_len = int(match.group(2))
    metadata_start = match.end()
    payload_start = metadata_start + metadata_len
    eof_pos = payload_start + payload_len
    frame_end = eof_pos + 1
    if len(data) < frame_end:
        if allow_partial:
            return None
        raise AssertionError(f"incomplete EV-MUX frame: need {frame_end}, got {len(data)}")
    if data[eof_pos:eof_pos + 1] != b"\x1f":
        raise AssertionError("EV-MUX frame missing EOF guard")

    metadata_bytes = data[metadata_start:payload_start]
    return EvMuxFrame(
        metadata=json.loads(metadata_bytes.decode("utf-8")),
        payload=data[payload_start:eof_pos],
        frame_start=match.start(),
        frame_end=frame_end,
    )


def parse_all_evmux(data: bytes) -> list[EvMuxFrame]:
    frames: list[EvMuxFrame] = []
    offset = 0
    while offset < len(data):
        frame = parse_first_evmux(data[offset:], allow_partial=True)
        if frame is None:
            break
        frames.append(
            EvMuxFrame(
                metadata=frame.metadata,
                payload=frame.payload,
                frame_start=offset + frame.frame_start,
                frame_end=offset + frame.frame_end,
            )
        )
        offset += frame.frame_end
    return frames


def read_until_frame(transport: Transport, predicate: Callable[[EvMuxFrame], bool], timeout: float) -> EvMuxFrame:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for index, pending_frame in enumerate(transport.pending_frames):
            if predicate(pending_frame):
                return transport.pending_frames.pop(index)

        while True:
            frame = transport.pop_frame()
            if frame is None:
                break
            if predicate(frame):
                return frame
            transport.pending_frames.append(frame)

        chunk = transport.serial.read(8192)
        if chunk:
            transport.frame_buffer.extend(chunk)
        else:
            time.sleep(0.02)
    raise TimeoutError(f"timed out waiting for EV-MUX frame; got {bytes(transport.frame_buffer[-500:])!r}")


def read_rpc(transport: Transport, method: str, timeout: float = 5) -> dict:
    frame = read_until_frame(
        transport,
        lambda item: item.metadata.get("channel") in ("user.rpc", "debug.rpc")
        and item.metadata.get("method") == method
        and item.metadata.get("type") in ("rsp", "event"),
        timeout=timeout,
    )
    payload = json.loads(frame.payload.decode("utf-8"))
    if not payload.get("ok"):
        raise AssertionError(f"{method} failed: {payload!r}")
    return payload


def read_rpc_error(transport: Transport, method: str, code: str, timeout: float = 5) -> dict:
    frame = read_until_frame(
        transport,
        lambda item: item.metadata.get("channel") in ("user.rpc", "debug.rpc")
        and item.metadata.get("method") == method
        and item.metadata.get("type") == "rsp",
        timeout=timeout,
    )
    payload = json.loads(frame.payload.decode("utf-8"))
    error = payload.get("error", {})
    if payload.get("ok") or error.get("code") != code:
        raise AssertionError(f"expected {method} error {code}, got {payload!r}")
    return payload


def read_event(transport: Transport, method: str, timeout: float = 5) -> dict:
    frame = read_until_frame(
        transport,
        lambda item: item.metadata.get("channel") in ("user.rpc", "debug.rpc")
        and item.metadata.get("method") == method
        and item.metadata.get("type") == "event",
        timeout=timeout,
    )
    payload = json.loads(frame.payload.decode("utf-8"))
    if not payload.get("ok"):
        raise AssertionError(f"{method} event failed: {payload!r}")
    return payload


def assert_firmware_identity(payload: dict, context: str) -> str:
    firmware = payload.get("firmware", {})
    if firmware.get("id") != "esp-vision":
        raise AssertionError(f"unexpected {context} firmware identity: {payload!r}")
    version = firmware.get("version")
    if not isinstance(version, str) or not version or version == "unknown":
        raise AssertionError(f"missing {context} firmware version: {payload!r}")
    return version


def run(args: argparse.Namespace) -> None:
    output = Path(args.output) if args.output else None
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)

    transport = Transport(args.port, args.baudrate)
    try:
        # Protocol v3: EV-MUX is up from boot and holding this port open makes
        # it the active sink — no raw-REPL bootstrap, no activation. Kill
        # whatever is running, then prove the framed REPL is live.
        transport.send_rpc("hello", {})
        hello = read_rpc(transport, "hello")
        if hello.get("evMuxVersion") != 3:
            raise AssertionError(f"unexpected hello payload: {hello!r}")
        firmware_version = assert_firmware_identity(hello, "hello")
        for removed_field in ("minEvMuxVersion", "evAtpVersion", "minEvAtpVersion"):
            if removed_field in hello:
                raise AssertionError(f"removed version field still advertised {removed_field}: {hello!r}")
        transport.serial.write(transport.frame("repl.signal", "data", None, b"\x03"))
        transport.send_repl('print("EV_TRANSPORT_BOOT_OK")\r\n')
        read_until_frame(
            transport,
            lambda item: item.metadata.get("channel") == "repl.stdout" and b"EV_TRANSPORT_BOOT_OK" in item.payload,
            timeout=8,
        )
        print(f"EV-MUX bootstrap (DTR route): ok firmware={firmware_version}")

        transport.send_rpc("capabilities", {})
        capabilities = read_rpc(transport, "capabilities")
        if capabilities.get("protocol", {}).get("evMuxVersion") != 3:
            raise AssertionError(f"unexpected capabilities protocol: {capabilities!r}")
        if assert_firmware_identity(capabilities, "capabilities") != firmware_version:
            raise AssertionError(f"firmware version changed during negotiation: {capabilities!r}")
        if capabilities.get("streams") != ["user", "debug"]:
            raise AssertionError(f"unexpected capabilities streams: {capabilities!r}")
        channels = capabilities.get("channels", [])
        if "user.rpc" not in channels or "host.rpc" in channels or "debug.data" in channels:
            raise AssertionError(f"unexpected capabilities channels: {capabilities!r}")
        for sink in capabilities.get("sinks", []):
            if {"work", "debug", "msc", "control", "data"} & set(sink):
                raise AssertionError(f"v2 sink booleans leaked into v3 capabilities: {sink!r}")
            if "kind" not in sink or "state" not in sink or "ready" not in sink:
                raise AssertionError(f"unexpected v3 sink entry: {sink!r}")
        for feature in (
            "transport.state",
            "route.get",
            "route.changed",
            "route.bind",
            "route.auto",
            "framed.repl",
            "script.write",
            "script.write.chunked",
            "script.run",
            "debug.info",
            "debug.capture_frame",
            "device.control",
            "dtr.routing",
        ):
            if feature not in capabilities.get("features", []):
                raise AssertionError(f"missing capability {feature}: {capabilities!r}")
        for removed in (
            "transport.activate",
            "transport.release",
            "work.activate",
            "work.heartbeat",
            "work.release",
            "rpc.split",
        ):
            if removed in capabilities.get("features", []):
                raise AssertionError(f"removed capability still advertised {removed}: {capabilities!r}")
        print("capabilities: ok")

        # Protocol v3: no lease, no activation. The port this test holds open
        # is the active sink; transport.state carries no lease fields.
        transport.send_rpc("transport.state", {})
        transport_state = read_rpc(transport, "transport.state")
        user = transport_state.get("user", {})
        if user.get("route") not in ("cdc", "usj") or not user.get("ready"):
            raise AssertionError(f"unexpected transport.state payload: {transport_state!r}")
        if "leaseRemainingMs" in transport_state.get("sinks", {}).get("cdc", {}):
            raise AssertionError(f"lease field leaked into v3 transport.state: {transport_state!r}")
        print(f"transport.state: ok user={user.get('route')}")

        for removed in ("transport.activate", "transport.release", "work.activate", "work.heartbeat", "work.release"):
            transport.send_rpc(removed, {"sink": "cdc", "leaseMs": 5000})
            read_rpc_error(transport, removed, "UNKNOWN_METHOD")
        print("removed lease RPCs return UNKNOWN_METHOD: ok")

        if user.get("route") == "cdc":
            # Dropping and re-raising DTR is the v3 activate/release: while
            # the port is closed every stream falls back to USJ (observable
            # only from the USJ side); on reopen the streams route back here.
            # Verify with the authoritative query (hello/transport.state):
            # route.changed events emitted at the DTR edge are best-effort and
            # can precede host read readiness — the IDE likewise probes
            # actively instead of trusting edge events.
            transport.close()
            time.sleep(1.0)
            transport = Transport(args.port, args.baudrate)
            transport.send_rpc("hello", {})
            read_rpc(transport, "hello", timeout=8)
            deadline = time.monotonic() + 8
            while True:
                transport.serial.write(transport.frame("debug.rpc", "req", "transport.state", b"{}"))
                state = read_rpc(transport, "transport.state", timeout=max(0.1, deadline - time.monotonic()))
                if state.get("user", {}).get("route") == "cdc":
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"user route did not return to cdc after reopen: {state!r}")
            print("port close/reopen: user/debug routes back to cdc after DTR re-raise: ok")

        for scope in ("device", "memory", "sensor"):
            transport.send_rpc("debug.info", {"scope": scope})
            payload = read_rpc(transport, "debug.info")
            if payload.get("scope") != scope:
                raise AssertionError(f"unexpected debug.info {scope}: {payload!r}")
            if scope == "device" and assert_firmware_identity(payload, "debug.info device") != firmware_version:
                raise AssertionError(f"inconsistent debug.info firmware version: {payload!r}")
            print(f"debug.info {scope}: ok")

        # Exercise the full path contract. script.write stages through
        # <path>.tmp, so the maximum accepted target path is 123 bytes while
        # the resulting VFS path remains within MICROPY_ALLOC_PATH_MAX.
        script_path = "/ev_transport_response_boundary_" + ("x" * 88) + ".py"
        if len(script_path) != 123:
            raise AssertionError(f"unexpected boundary path length: {len(script_path)}")
        too_long_path = "/ev_transport_response_boundary_" + ("x" * 89) + ".py"
        if len(too_long_path) != 124:
            raise AssertionError(f"unexpected rejected path length: {len(too_long_path)}")
        script = 'print("EV_TRANSPORT_SCRIPT_OK")\n'
        transport.send_rpc(
            "script.write",
            {
                "path": too_long_path,
                "encoding": "utf-8",
                "mode": "overwrite",
                "contentBase64": base64.b64encode(script.encode("utf-8")).decode("ascii"),
            },
        )
        read_rpc_error(transport, "script.write", "PATH_TOO_LONG")
        print("script.write path limit: ok")
        transport.send_rpc(
            "script.write",
            {
                "path": "/ev_transport/../escape.py",
                "encoding": "utf-8",
                "mode": "overwrite",
                "contentBase64": base64.b64encode(script.encode("utf-8")).decode("ascii"),
            },
        )
        read_rpc_error(transport, "script.write", "INVALID_PATH")
        print("script.write path validation: ok")
        transport.send_rpc(
            "debug.info",
            {
                "scope": "fs.list",
                "path": too_long_path,
            },
        )
        read_rpc_error(transport, "debug.info", "PATH_TOO_LONG")
        print("debug.info fs.list path limit: ok")
        transport.send_rpc(
            "script.write",
            {
                "path": script_path,
                "encoding": "utf-8",
                "mode": "overwrite",
                "contentBase64": base64.b64encode(script.encode("utf-8")).decode("ascii"),
            },
        )
        write_payload = read_rpc(transport, "script.write")
        if write_payload.get("bytes") != len(script):
            raise AssertionError(f"unexpected script.write payload: {write_payload!r}")
        print("script.write: ok")

        transport.send_rpc("debug.info", {"scope": "fs.read", "path": script_path})
        file_payload = read_rpc(transport, "debug.info")
        file_data = base64.b64decode(file_payload["contentBase64"]).decode("utf-8")
        if file_data != script:
            raise AssertionError(f"unexpected file data: {file_data!r}")
        print("debug.info fs.read: ok")

        transport.send_rpc("debug.info", {"scope": "fs.list", "path": "/"})
        list_payload = read_rpc(transport, "debug.info")
        if script_path.lstrip("/") not in list_payload.get("entries", []):
            raise AssertionError(f"missing script in fs.list: {list_payload!r}")
        print("debug.info fs.list: ok")

        chunked_path = "/ev_transport_chunked.py"
        chunked_script = "# chunked script.write validation\n" + ("print('EV_TRANSPORT_CHUNKED_OK')\n" * 90)
        chunk_bytes = chunked_script.encode("utf-8")
        chunk_size = 900
        for offset in range(0, len(chunk_bytes), chunk_size):
            chunk = chunk_bytes[offset:offset + chunk_size]
            transport.send_rpc(
                "script.write",
                {
                    "path": chunked_path,
                    "encoding": "utf-8",
                    "mode": "overwrite",
                    "offset": offset,
                    "totalBytes": len(chunk_bytes),
                    "contentBase64": base64.b64encode(chunk).decode("ascii"),
                },
            )
            chunk_payload = read_rpc(transport, "script.write")
            expected_complete = (offset + len(chunk)) == len(chunk_bytes)
            if chunk_payload.get("complete") is not expected_complete:
                raise AssertionError(f"unexpected chunked write payload: {chunk_payload!r}")
        transport.send_rpc("debug.info", {"scope": "fs.read", "path": chunked_path})
        chunked_file_payload = read_rpc(transport, "debug.info")
        chunked_file_data = base64.b64decode(chunked_file_payload["contentBase64"]).decode("utf-8")
        if chunked_file_data != chunked_script[:len(chunked_file_data)]:
            raise AssertionError("unexpected chunked file prefix")
        print("script.write chunked: ok")

        transport.send_rpc(
            "script.write",
            {
                "path": script_path,
                "encoding": "utf-8",
                "mode": "create",
                "contentBase64": base64.b64encode(script.encode("utf-8")).decode("ascii"),
            },
        )
        read_rpc_error(transport, "script.write", "FILE_EXISTS")
        print("script.write create-existing error: ok")

        transport.send_rpc("script.run", {"path": script_path})
        read_rpc(transport, "script.run")
        read_until_frame(
            transport,
            lambda item: item.metadata.get("channel") == "repl.stdout" and b"EV_TRANSPORT_SCRIPT_OK" in item.payload,
            timeout=5,
        )
        print("script.run: ok")

        transport.send_rpc("debug.capture_frame", {"quality": args.quality})
        # Protocol v3: debug.data is deleted — the JPEG travels as the binary
        # payload of the debug.rpc response frame itself (not JSON).
        image_frame = read_until_frame(
            transport,
            lambda item: item.metadata.get("channel") == "debug.rpc"
            and item.metadata.get("method") == "debug.capture_frame"
            and item.metadata.get("type") == "rsp"
            and item.metadata.get("encoding") == "binary",
            timeout=args.frame_timeout,
        )
        if image_frame.metadata.get("contentType") != "image/jpeg":
            raise AssertionError(f"unexpected capture_frame metadata: {image_frame.metadata!r}")
        if not image_frame.metadata.get("width") or not image_frame.metadata.get("height"):
            raise AssertionError(f"capture_frame metadata missing dimensions: {image_frame.metadata!r}")
        if not image_frame.payload.startswith(b"\xff\xd8") or not image_frame.payload.endswith(b"\xff\xd9"):
            raise AssertionError("debug.capture_frame payload is not a complete JPEG")
        if output is not None:
            output.write_bytes(image_frame.payload)
            print(f"debug.capture_frame: ok saved={output} bytes={len(image_frame.payload)}")
        else:
            print(f"debug.capture_frame: ok bytes={len(image_frame.payload)}")

        transport.send_rpc("device.control", {"action": "status"})
        status_payload = read_rpc(transport, "device.control")
        if status_payload.get("action") != "status" or "user" not in status_payload:
            raise AssertionError(f"unexpected device.control status: {status_payload!r}")
        print("device.control status: ok")

        cleanup_source = (
            "import os\n"
            f"for p in {(script_path, chunked_path)!r}:\n"
            " try: os.remove(p)\n"
            " except OSError: pass\n"
            "print('EV_TRANSPORT_CLEANUP_OK')"
        )
        transport.send_repl(f"exec({cleanup_source!r})\r\n")
        read_until_frame(
            transport,
            lambda item: item.metadata.get("channel") == "repl.stdout"
            and b"EV_TRANSPORT_CLEANUP_OK" in item.payload,
            timeout=5,
        )
        print("script test cleanup: ok")

        # Leave the device at a quiet REPL; EV-MUX stays enabled (boot
        # default) for the next run.
        transport.serial.write(transport.frame("repl.signal", "data", None, b"\x03"))
        time.sleep(0.2)
    finally:
        transport.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--quality", type=int, default=60)
    parser.add_argument("--frame-timeout", type=float, default=25)
    parser.add_argument("--output", help="optional path for the captured JPEG")
    args = parser.parse_args()

    try:
        run(args)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
