#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Flash a firmware build onto the attached board and run the hardware tests.

Driver for the hardware-in-the-loop CI jobs (.gitlab/ci/target_test.yml). The
firmware build job hands over its ``build/<BOARD>/idf*/`` artifacts; this script
flashes them with esptool, waits until EV-MUX answers again, and then runs, in
order:

  * ``tools/test_control_transport.py`` - the EV-MUX transport contract,
  * ``tools/ci/device_checks.py`` - the camera (capture, framesizes, pixel
    formats, orientation, frame rate, host preview, imlib) and the LCD.

Everything printed here and by those two scripts is mirrored into a log file
(``--log``) that the job keeps as an artifact, and each stage ends up in a
summary table with its status and duration.

Flash parameters come from the build's ``flasher_args.json`` (chip, reset
behaviour, flash settings, offsets), so no board repeats its offsets here.
esptool renamed its commands in v5; both spellings are emitted according to the
installed version.
"""

from __future__ import annotations

import argparse
import json
import os
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
except ImportError as exc:  # pragma: no cover - host dependency guard
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc

sys.path.insert(0, str(Path(__file__).resolve().parent))
import device_checks  # noqa: E402  (needs the path entry above)

REPO_ROOT = Path(__file__).resolve().parents[2]
TRANSPORT_TEST = REPO_ROOT / "tools" / "test_control_transport.py"
DEVICE_CHECKS = REPO_ROOT / "tools" / "ci" / "device_checks.py"
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
    named, generic = [], []
    for port in list_ports.comports():
        text = f"{port.product or ''} {port.description or ''} {port.manufacturer or ''}".upper()
        if "ESP-VISION" in text:
            named.append(port.device)
        elif port.vid == ESPRESSIF_VID:
            generic.append(port.device)
    return named + generic


def describe_ports(explicit: Optional[str]) -> None:
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
    print(f"build      : {build_dir.relative_to(REPO_ROOT)}")

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
    print(f"host tools : esptool {esptool_version()}, pyserial {serial.__version__}, "
          f"python {sys.version.split()[0]}")
    print(f"flash      : chip {flasher.get('extra_esptool_args', {}).get('chip')}, "
          f"{flasher.get('flash_settings', {})}, {args.baud} baud")
    print("serial ports visible to this job:")
    describe_ports(args.port)
    print("=" * 78)


def wait_for_device(args: argparse.Namespace) -> str:
    """Poll until the freshly flashed board answers EV-MUX again."""
    deadline = time.monotonic() + args.boot_timeout
    attempts = 0
    while time.monotonic() < deadline:
        for port in candidate_ports(args.port):
            attempts += 1
            if device_checks.answers_hello(port, args.baudrate, timeout=2.0):
                print(f"board answered EV-MUX on {port} after {attempts} probe(s)", flush=True)
                return port
        time.sleep(1.0)
    raise SystemExit(f"board did not answer EV-MUX within {args.boot_timeout:.0f}s ({attempts} probes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", required=True, help="board name, as in boards/<BOARD>")
    parser.add_argument("--build-dir", help="build output to flash; defaults to build/<BOARD>/idf*")
    parser.add_argument("--port", default=os.environ.get("ESPPORT") or None,
                        help="serial port; auto-detected when omitted")
    parser.add_argument("--chip", help="override the chip from flasher_args.json")
    parser.add_argument("--baud", type=int, default=int(os.environ.get("ESPBAUD", "460800")),
                        help="esptool flashing baud rate")
    parser.add_argument("--baudrate", type=int, default=115200, help="serial baud rate used by the tests")
    parser.add_argument("--erase", action="store_true", help="erase the whole flash before writing")
    parser.add_argument("--output", help="path for the JPEG captured by the checks")
    parser.add_argument("--log", help="path for the full text log; defaults to logs/<BOARD>-target-test.log")
    parser.add_argument("--checks", default="all", help="checks forwarded to device_checks.py")
    parser.add_argument("--frames", type=int, default=30, help="frames used by the timing checks")
    parser.add_argument("--settle", type=float, default=3.0, help="seconds to wait for USB re-enumeration")
    parser.add_argument("--boot-timeout", type=float, default=90.0, help="seconds to wait for EV-MUX after reset")
    args = parser.parse_args()

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

        # Both suites run even if the first one fails: a full log is worth more
        # than an early exit when the board is on the other side of the runner.
        transport_ok = stage(
            "transport test (tools/test_control_transport.py)",
            [sys.executable, str(TRANSPORT_TEST), "--port", port, "--baudrate", str(args.baudrate)],
        )
        checks = [sys.executable, str(DEVICE_CHECKS), "--port", port, "--baudrate", str(args.baudrate),
                  "--checks", args.checks, "--frames", str(args.frames)]
        if args.output:
            checks += ["--output", args.output]
        checks_ok = stage("device checks (tools/ci/device_checks.py)", checks)
        failed = not (transport_ok and checks_ok)
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
