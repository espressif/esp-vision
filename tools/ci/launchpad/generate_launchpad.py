#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Merge per-board ESP-VISION firmware and emit an esp-launchpad config.toml.

This runs in the tag-gated ``pack_launchpad`` CI job. It reuses the artifacts
produced by the ``firmware`` build matrix (``build/<BOARD>/idf*/``) rather than
rebuilding, merges each board into a single flashable image whose name is
suffixed with the release tag, and writes ``binaries/config.toml`` describing
them for https://espressif.github.io/esp-launchpad/.

Because every published binary carries the tag in its filename and the uploader
copies additively (``aws s3 cp --recursive``, no ``--delete``), older releases
remain downloadable; only ``config.toml`` is overwritten to point launchpad at
the newest tag.

Usage:
    generate_launchpad.py <tag>
"""

import glob
import os
import re
import shutil
import subprocess
import sys

import rtoml
import yaml

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BOARDS_FILE = os.path.join(SCRIPT_DIR, "boards.yml")
OUTPUT_DIR = "binaries"


def find_build_dir(board: str) -> str:
    """Return the build directory for a board, e.g. build/<board>/idf5.5."""
    candidates = sorted(glob.glob(os.path.join("build", board, "idf*")))
    candidates = [d for d in candidates if os.path.isfile(os.path.join(d, "flash_args"))]
    if not candidates:
        return ""
    # If several IDF versions are present, publish the highest one.
    return candidates[-1]


def merge_board(board: str, target: str, tag: str, build_dir: str) -> str:
    """Merge a single board's firmware into binaries/ and return the bin name."""
    bin_name = f"esp-vision-{board}-{tag}.bin"
    print(f"[{board}] merging {build_dir} -> {bin_name}")
    subprocess.run(
        ["esptool.py", "--chip", target, "merge_bin", "-o", bin_name, "@flash_args"],
        cwd=build_dir,
        check=True,
    )
    shutil.move(os.path.join(build_dir, bin_name), os.path.join(OUTPUT_DIR, bin_name))
    return bin_name


def build_toml(cfg: dict, images: dict) -> dict:
    """Assemble the esp-launchpad config.toml object.

    images maps board -> (target, bin_name).
    """
    app = cfg["app_name"]
    app_obj: dict = {
        "android_app_url": "",
        "ios_app_url": "",
        "chipsets": [],
    }
    for board, (target, bin_name) in images.items():
        if target not in app_obj["chipsets"]:
            app_obj["chipsets"].append(target)
        app_obj.setdefault(f"developKits.{target}", []).append(board)
        app_obj[f"image.{board}"] = bin_name

    return {
        "esp_toml_version": 1.0,
        "firmware_images_url": cfg["firmware_images_url"],
        "supported_apps": [app],
        app: app_obj,
    }


def unquote_keys(text: str) -> str:
    """Strip the quotes rtoml adds around dotted launchpad keys."""
    return re.sub(r'"((image|developKits)\.[\w-]+)"', r"\1", text)


def main() -> None:
    if len(sys.argv) != 2 or not sys.argv[1]:
        sys.exit("usage: generate_launchpad.py <tag>")
    tag = sys.argv[1]

    with open(BOARDS_FILE) as f:
        cfg = yaml.safe_load(f)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    images = {}
    for board, meta in cfg["boards"].items():
        target = meta["target"]
        build_dir = find_build_dir(board)
        if not build_dir:
            if meta.get("optional", False):
                print(f"[{board}] no build artifacts found (optional) -> skipping")
                continue
            raise RuntimeError(
                f"no build dir with flash_args for {board}; "
                f"is the firmware:[...,{board}] job a dependency?"
            )
        bin_name = merge_board(board, target, tag, build_dir)
        images[board] = (target, bin_name)

    if not images:
        raise RuntimeError("no boards merged; refusing to write an empty config.toml")

    toml_obj = build_toml(cfg, images)
    text = unquote_keys(rtoml.dumps(toml_obj))
    out = os.path.join(OUTPUT_DIR, "config.toml")
    with open(out, "w") as f:
        f.write(text)

    print(f"\nwrote {out}:\n{text}")


if __name__ == "__main__":
    main()
