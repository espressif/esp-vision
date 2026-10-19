#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Flash a firmware build onto the attached board and run the hardware tests.

Driver for the hardware-in-the-loop CI jobs (.gitlab/ci/target_test.yml). The
firmware build job hands over its ``build/<BOARD>/idf*/`` artifacts; this script
flashes them with esptool, then invokes the host SDK's single hardware CLI:
``host/device-core/tests/hardware-check.mjs``. The same public ``createDevice``
entry handles startup probes, device identity, Python, camera and display checks.
Python only enumerates serial ports, flashes images and launches Node; it does
not implement or parse EV-MUX.

Output from the flashing and SDK checks is mirrored into an artifact log
(``--log``), and each stage records its status and duration.

Flash parameters come from the build's ``flasher_args.json`` (chip, reset
behaviour, flash settings, offsets), so no board repeats its offsets here.
esptool renamed its commands in v5; both spellings are emitted according to the
installed version.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - hardware-job dependency guard
    serial = None
    list_ports = None

REPO_ROOT = Path(__file__).resolve().parents[2]
HARDWARE_CHECK = REPO_ROOT / "host" / "device-core" / "tests" / "hardware-check.mjs"
ESPRESSIF_VID = 0x303A


@dataclass
class Stage:
    name: str
    status: str
    seconds: float


class Tee:
    """Mirror this job's output into the artifact log as well as the console."""

    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = path.open("w", encoding="utf-8")
        self.console = sys.stdout

    def write(self, text: str) -> int:
        self.console.write(text)
        self.file.write(text)
        return len(text)

    def flush(self) -> None:
        self.console.flush()
        self.file.flush()

    def close(self) -> None:
        self.file.close()


def resolve_build_dir(board: str, explicit: Optional[str]) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_dir():
            raise SystemExit(f"build directory not found: {path}")
        return path
    candidates = [path for path in sorted((REPO_ROOT / "build" / board).glob("idf*"))
                  if (path / "flasher_args.json").is_file()]
    if not candidates:
        raise SystemExit(f"no build output with flasher_args.json under build/{board}/idf*")
    if len(candidates) > 1:
        raise SystemExit(
            f"several builds under build/{board}: {', '.join(path.name for path in candidates)}; "
            "select one with --build-dir"
        )
    return candidates[0]


def esptool_names() -> dict[str, str]:
    """esptool v5 hyphenates command and reset-mode names; v4 uses underscores."""
    try:
        import esptool
    except ImportError as exc:
        raise SystemExit("esptool is required: python3 -m pip install 'esptool>=5.3'") from exc
    head = getattr(esptool, "__version__", "0").split(".")[0]
    if head.isdigit() and int(head) >= 5:
        return {"write": "write-flash", "erase": "erase-flash", "separator": "-"}
    return {"write": "write_flash", "erase": "erase_flash", "separator": "_"}


def esptool_base(args: argparse.Namespace, flasher: dict) -> list[str]:
    chip = args.chip or flasher.get("extra_esptool_args", {}).get("chip", "auto")
    command = [sys.executable, "-m", "esptool", "--chip", chip]
    if args.port:
        command += ["--port", args.port]
    return command + ["--baud", str(args.baud)]


def flash_command(args: argparse.Namespace, build_dir: Path, flasher: dict) -> list[str]:
    names = esptool_names()
    extra = flasher.get("extra_esptool_args", {})
    settings = flasher.get("flash_settings", {})

    def spell(value: str) -> str:
        # flasher_args.json spells these the way its own IDF version does.
        return value.replace("-", "_").replace("_", names["separator"])

    command = esptool_base(args, flasher) + [
        "--before", spell(extra.get("before") or "default-reset"),
        "--after", spell(extra.get("after") or "hard-reset"),
        names["write"],
    ]
    for key in ("flash_mode", "flash_freq", "flash_size"):
        value = settings.get(key)
        if value:
            command += [f"--{spell(key)}", value]
    for offset, relative in sorted(flasher.get("flash_files", {}).items(), key=lambda item: int(item[0], 16)):
        image = build_dir / relative
        if not image.is_file():
            raise SystemExit(f"missing flash image {image} referenced by flasher_args.json")
        command += [offset, str(image)]
    return command


def erase_command(args: argparse.Namespace, flasher: dict) -> list[str]:
    return esptool_base(args, flasher) + [esptool_names()["erase"]]


def run(command: list[str]) -> int:
    """Run a command, streaming its output through the tee, and return its code."""
    print("+ " + " ".join(command), flush=True)
    process = subprocess.Popen(command, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in process.stdout:
        print(line.rstrip("\n"), flush=True)
    return process.wait()


def candidate_ports(explicit: Optional[str]) -> list[str]:
    if explicit:
        return [explicit]
    if list_ports is None:
        return []
    named, generic = [], []
    for port in list_ports.comports():
        text = f"{port.product or ''} {port.description or ''} {port.manufacturer or ''}".upper()
        if "ESP-VISION" in text:
            named.append(port.device)
        elif port.vid == ESPRESSIF_VID:
            generic.append(port.device)
    return named + generic


def describe_ports(explicit: Optional[str]) -> None:
    if list_ports is None:
        print("  (pyserial is not installed)")
        return
    listed = False
    for port in list_ports.comports():
        if port.vid is None and port.device != explicit:
            continue
        vid = f"{port.vid:04X}" if port.vid else "----"
        pid = f"{port.pid:04X}" if port.pid else "----"
        print(f"  {port.device}  {vid}:{pid}  {port.product or port.description or ''}")
        listed = True
    if not listed:
        print("  (no USB serial device visible)")


def esptool_version() -> str:
    try:
        import esptool
    except ImportError:
        return "missing"
    return getattr(esptool, "__version__", "unknown")


def print_context(args: argparse.Namespace, build_dir: Path, flasher: dict) -> None:
    print("=" * 78)
    print(f"target test: {args.board}")
    print(f"started    : {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')} UTC")
    print(f"build      : {os.path.relpath(build_dir, REPO_ROOT)}")

    config = build_dir / "config.env"
    if config.is_file():
        try:
            env = json.loads(config.read_text())
            print(f"target     : {env.get('IDF_TARGET')} / ESP-IDF {env.get('IDF_VERSION')}")
        except ValueError:
            pass
    app = flasher.get("app", {}).get("file")
    if app and (build_dir / app).is_file():
        print(f"app image  : {app}, {(build_dir / app).stat().st_size / 1024:.0f} KiB")
    commit = os.environ.get("CI_COMMIT_SHORT_SHA") or os.environ.get("CI_COMMIT_SHA", "")[:8]
    if commit:
        print(f"commit     : {commit} ({os.environ.get('CI_COMMIT_REF_NAME', 'local')})")
    serial_version = serial.__version__ if serial is not None else "missing"
    print(f"host tools : esptool {esptool_version()}, pyserial {serial_version}, "
          f"python {sys.version.split()[0]}")
    print(f"flash      : chip {flasher.get('extra_esptool_args', {}).get('chip')}, "
          f"{flasher.get('flash_settings', {})}, {args.baud} baud")
    print("serial ports visible to this job:")
    describe_ports(args.port)
    print("=" * 78)


def serial_role(port: str, explicit: Optional[str] = None) -> str:
    """Select the SDK role from the port descriptor, without probing a protocol."""
    if explicit:
        return explicit
    text = port.upper()
    if list_ports is not None:
        for item in list_ports.comports():
            if item.device == port or os.path.realpath(item.device) == os.path.realpath(port):
                text += f" {item.product or ''} {item.description or ''}".upper()
                break
    if "JTAG" in text:
        return "usj"
    if "ESP-VISION" in text:
        return "usb"
    return "usj"


def hardware_check_command(args: argparse.Namespace, port: str, *, probe: bool = False,
                           timeout_ms: int = 12000) -> list[str]:
    command = [args.node, str(HARDWARE_CHECK), "--serial", port,
               "--role", serial_role(port, args.role), "--python", sys.executable,
               "--expect-board", args.board]
    if args.expected_version:
        command += ["--expect-firmware-version", args.expected_version]
    if probe:
        return command + ["--timeout", str(timeout_ms)]
    command += ["--checks", args.checks, "--frames", str(args.frames),
                "--display-hold", str(args.display_hold)]
    if args.require_all:
        command += ["--require-all"]
    if args.output:
        command += ["--output", str(args.output)]
    if args.screen_output:
        command += ["--screen-output", str(args.screen_output)]
    return command


def wait_for_device(args: argparse.Namespace) -> str:
    """Poll the public SDK CLI until identity and Python both work after reset."""
    deadline = time.monotonic() + args.boot_timeout
    attempts = 0
    while time.monotonic() < deadline:
        for port in candidate_ports(args.port):
            remaining = deadline - time.monotonic()
            if remaining < 1:
                break
            attempts += 1
            timeout_ms = min(12000, int(remaining * 1000))
            command = hardware_check_command(args, port, probe=True, timeout_ms=timeout_ms)
            try:
                result = subprocess.run(command, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True,
                                        timeout=timeout_ms / 1000 + 12)
            except subprocess.TimeoutExpired as exc:
                output = exc.stdout or ""
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
                if output:
                    print(output.rstrip(), flush=True)
                print(f"SDK startup probe {attempts} timed out on {port}", flush=True)
                continue
            if result.returncode == 0:
                print(f"SDK identity and Python ready on {port} after {attempts} probe(s)", flush=True)
                for line in result.stdout.splitlines():
                    print(line, flush=True)
                return port
            print(f"SDK startup probe {attempts} failed on {port} (exit {result.returncode})", flush=True)
            if result.stdout:
                print(result.stdout.rstrip(), flush=True)
        time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
    raise SystemExit(f"board did not pass SDK identity/Python checks within {args.boot_timeout:.0f}s "
                     f"({attempts} probes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", required=True, help="board name, as in boards/<BOARD>")
    parser.add_argument("--build-dir", help="build output to flash; defaults to build/<BOARD>/idf*")
    parser.add_argument("--port", default=os.environ.get("ESPPORT") or None,
                        help="serial port; auto-detected when omitted")
    parser.add_argument("--chip", help="override the chip from flasher_args.json")
    parser.add_argument("--baud", type=int, default=int(os.environ.get("ESPBAUD", "460800")),
                        help="esptool flashing baud rate")
    parser.add_argument("--baudrate", type=int, default=115200,
                        help="SDK serial adapter baud rate (currently fixed at 115200)")
    parser.add_argument("--role", choices=("usj", "usb", "uart"),
                        help="SDK serial role; inferred from USB descriptors when omitted")
    parser.add_argument("--node", default=os.environ.get("NODE", "node"), help="Node.js 22+ executable")
    parser.add_argument("--erase", action="store_true", help="erase the whole flash before writing")
    parser.add_argument("--output", help="path for the JPEG captured by the checks")
    parser.add_argument("--screen-output", help="path for the LCD test-pattern JPEG captured through Core")
    parser.add_argument("--log", help="path for the full text log; defaults to logs/<BOARD>-target-test.log")
    parser.add_argument("--checks", default="all", help="all or comma-separated checks forwarded to the SDK hardware CLI")
    parser.add_argument("--require-all", action="store_true", help="fail if any selected check is unsupported instead of accepting a skip")
    parser.add_argument("--frames", type=int, default=30, help="frames used by the timing checks")
    parser.add_argument("--display-hold", type=int, default=0,
                        help="seconds to hold each LCD backlight state for visual inspection (default: 0)")
    parser.add_argument("--settle", type=float, default=3.0, help="seconds to wait for USB re-enumeration")
    parser.add_argument("--boot-timeout", type=float, default=90.0, help="seconds to wait for SDK identity and Python after reset")
    args = parser.parse_args()
    if args.display_hold < 0:
        parser.error("--display-hold must be non-negative")
    if args.baudrate != 115200:
        parser.error("the SDK serial adapter currently requires --baudrate 115200")
    if not shutil.which(args.node):
        parser.error("Node.js 22+ is required; select it with --node or NODE")
    node_version = subprocess.run([args.node, "-e", "process.exit(Number(process.versions.node.split('.')[0]) >= 22 ? 0 : 1)"],
                                  capture_output=True, text=True)
    if node_version.returncode:
        parser.error("Node.js 22+ is required by Device Core")
    if serial is None:
        parser.error("pyserial is required: python3 -m pip install pyserial")

    log_path = Path(args.log) if args.log else REPO_ROOT / "logs" / f"{args.board}-target-test.log"
    tee = Tee(log_path)
    sys.stdout = tee
    started = time.monotonic()
    stages: list[Stage] = []

    def stage(name: str, command: list[str]) -> bool:
        print(f"\n----- {name} -----")
        stage_started = time.monotonic()
        code = run(command)
        seconds = time.monotonic() - stage_started
        stages.append(Stage(name, "PASS" if code == 0 else f"FAIL ({code})", seconds))
        print(f"----- {name}: {'PASS' if code == 0 else f'FAIL, exit {code}'} in {seconds:.1f}s -----")
        return code == 0

    failed = True
    try:
        build_dir = resolve_build_dir(args.board, args.build_dir)
        flasher = json.loads((build_dir / "flasher_args.json").read_text())
        description = build_dir / "project_description.json"
        args.expected_version = None
        if description.is_file():
            args.expected_version = json.loads(description.read_text()).get("project_version")
        if os.environ.get("CI") and not args.expected_version:
            raise SystemExit("CI requires project_description.json with the expected firmware version")
        print_context(args, build_dir, flasher)

        # Fail with a readable message when the board is simply not visible
        # here, instead of letting esptool report a port it never had.
        if args.port and not Path(args.port).exists():
            raise SystemExit(f"{args.port} does not exist; is the board passed into this job?")
        if not candidate_ports(args.port):
            raise SystemExit(
                "no Espressif serial port found; set ESPPORT, or make sure the runner passes the "
                "board's device node into the job"
            )

        if args.erase and not stage("erase flash", erase_command(args, flasher)):
            raise SystemExit("erase failed; skipping the tests")
        if not stage("flash firmware", flash_command(args, build_dir, flasher)):
            raise SystemExit("flashing failed; skipping the tests")

        print(f"\nwaiting {args.settle:.0f}s for USB re-enumeration")
        time.sleep(args.settle)
        port = wait_for_device(args)

        checks_ok = stage("Device Core hardware checks", hardware_check_command(args, port))
        failed = not checks_ok
    except SystemExit as exc:
        # Keep the abort reason inside the artifact log, not only on stderr.
        print(f"\nABORTED: {exc}")
    finally:
        print("\nstage summary")
        for item in stages:
            print(f"  {item.status:<10} {item.seconds:6.1f}s  {item.name}")
        print(f"  total {time.monotonic() - started:.1f}s, log written to {log_path}")
        sys.stdout = tee.console
        tee.close()

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
