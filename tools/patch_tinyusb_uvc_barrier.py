#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""Apply the ESP-VISION UVC lifetime patch to a copied TinyUSB component."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
PATCH_FILE = REPO_ROOT / "tools/patches/tinyusb-uvc-lifetime.patch"
MARKERS = {
    "src/class/video/video_device.c": "tud_video_n_frame_xfer_tagged",
    "src/class/video/video_device.h": "tud_video_stream_closed_cb",
    "src/device/dcd.h": "USBD_EVENT_ENDPOINT_QUIESCED",
    "src/device/usbd.c": "usbd_event_endpoint_quiesced",
    "src/device/usbd_pvt.h": "usbd_event_endpoint_quiesced",
    "src/portable/synopsys/dwc2/dcd_dwc2.c": "dcd_edpt_quiesce_task",
}


class PatchError(Exception):
    pass


def patch_state(component_root: Path) -> str:
    matches = []
    for relative, marker in MARKERS.items():
        path = component_root / relative
        if not path.is_file():
            raise PatchError(f"missing expected TinyUSB source: {path}")
        matches.append(marker in path.read_text(encoding="utf-8"))
    if all(matches):
        return "applied"
    if any(matches):
        raise PatchError("component contains only part of the UVC lifetime patch")
    return "clean"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component_root", type=Path, help="copied TinyUSB component root containing src/")
    parser.add_argument("--check", action="store_true", help="validate patch applicability without applying it")
    args = parser.parse_args()

    component_root = args.component_root.resolve()
    if "managed_components" in component_root.parts:
        raise PatchError("refusing to patch managed_components; copy the component first")
    if not (component_root / "src").is_dir():
        raise PatchError(f"expected a copied component root containing src/: {component_root}")
    if not PATCH_FILE.is_file():
        raise PatchError(f"patch file is missing: {PATCH_FILE}")

    state = patch_state(component_root)
    if state == "applied":
        print(f"TinyUSB UVC lifetime patch already present: {component_root}")
        return 0

    patch_executable = shutil.which("patch")
    if patch_executable is None:
        raise PatchError("GNU-compatible 'patch' executable is required")

    temp_root = component_root / ".patch-tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["TMPDIR"] = str(temp_root)
    env["TMP"] = str(temp_root)
    env["TEMP"] = str(temp_root)

    command = [
        patch_executable,
        "--directory", str(component_root),
        "--strip=1",
        "--batch",
        "--forward",
        "--fuzz=0",
        "--input", str(PATCH_FILE),
    ]
    if args.check:
        command.insert(1, "--dry-run")

    result = subprocess.run(command, env=env, text=True, capture_output=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise PatchError("TinyUSB sources do not match the supported patch context")

    if args.check:
        print(f"TinyUSB UVC lifetime patch applies cleanly: {component_root}")
        return 0

    if patch_state(component_root) != "applied":
        raise PatchError("patch command completed without all expected markers")
    print(f"Applied TinyUSB UVC lifetime patch: {component_root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PatchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
