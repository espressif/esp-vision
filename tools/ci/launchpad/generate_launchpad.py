#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Merge qualified firmware and write a hash-bound immutable publication.

The standard launchpad fields and website snapshot remain for compatibility.
Real releases also write releases/<tag>/manifest.json and a [publication]
pointer with published-only history and a previous-config SHA for uploader CAS.
The default legacy layout retains full/app bins and per-board JSON. Explicit
single-bin mode binds application slices to complete containers instead.
Dry runs predict compatibility config only; they never qualify a publication.
"""

import argparse
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

import rtoml
import yaml

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BOARDS_FILE = os.path.join(SCRIPT_DIR, "boards.yml")
OUTPUT_DIR = "binaries"

# docs/ holds board_model.py + targets.py: the single source of derived board
# capability data, shared with the documentation board-support tables.
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "docs"))

from board_model import board_info  # noqa: E402

# Bundled .espdl models live here, each with a same-named .json metadata sidecar.
MODELS_DIR = os.path.join(REPO_ROOT, "models")
MODEL_DOWNLOAD_BASE_URL = "https://raw.githubusercontent.com/espressif/esp-vision/"

# Keys every model sidecar must define (see models/README.md). sizeBytes is
# authored but cross-checked against the actual .espdl below.
MODEL_REQUIRED_KEYS = (
    "name",
    "architecture",
    "api",
    "task",
    "input",
    "inputFormat",
    "dataset",
    "labels",
    "sizeBytes",
)

# A JSON anywhere under models/ is treated as a model sidecar when it carries
# these identifying keys. Any other JSON (not a model description) is ignored,
# so discovery is not tied to a particular subfolder or family.
MODEL_SENTINEL_KEYS = frozenset({"architecture", "api"})

# [website] schema version, semver "MAJOR.MINOR.PATCH". The website pins the
# MAJOR (integer before the first dot); additive changes bump MINOR so older
# consumers keep working by ignoring unknown keys, breaking changes bump MAJOR.
# Bump this whenever the [website] shape changes, and note it in the repo-root
# CHANGELOG.md.
WEBSITE_SCHEMA_VERSION = "1.2.0"

# board_model imlib feature labels -> website vision tokens. Unmapped labels
# fall back to a slugified label. This is website *presentation* shaping, not
# truth, so it lives here rather than in board_model.
VISION_TOKEN = {
    "pixel/math": "pixel-math",
    "filters": "filters",
    "geometry": "geometry",
    "template matching": "template-matching",
    "QR code": "qrcode",
    "AprilTag": "apriltag",
}


def _slugify(text: str) -> str:
    out = []
    for ch in text.strip().lower():
        if ch.isalnum():
            out.append(ch)
        elif ch in " _-/.":
            out.append("-")
    slug = "".join(out)
    while "--" in slug:
        slug = slug.replace("--", "-")
    return slug.strip("-")


def find_build_dir(board: str) -> str:
    """Return the build directory for a board, e.g. build/<board>/idf5.5."""
    candidates = sorted(glob.glob(os.path.join("build", board, "idf*")))
    candidates = [d for d in candidates if os.path.isfile(os.path.join(d, "flash_args"))]
    if not candidates:
        return ""
    # If several IDF versions are present, publish the highest one.
    return candidates[-1]


def bin_name_for(board: str, tag: str) -> str:
    """Deterministic published bin name; the only place this pattern is defined."""
    return f"esp-vision-{board}-{tag}.bin"


def bin_name_template() -> str:
    """The bin-name pattern as a ``{board}``/``{tag}`` template for the website.

    Derived from :func:`bin_name_for` so the pattern stays single-sourced: the
    site fills this and prefixes ``firmware_images_url`` to build any version's
    download URL itself.
    """
    return bin_name_for("{board}", "{tag}")


def load_merge_args(build_dir: str) -> list:
    """Load, normalize, and validate IDF's response-file merge arguments."""
    flash_args_path = os.path.join(build_dir, "flash_args")
    with open(flash_args_path, encoding="utf-8") as f:
        args = shlex.split(f.read())

    # esptool 5.3 keeps these aliases for compatibility but warns that they
    # will be removed. Normalize the IDF-generated response file at the release
    # boundary so the packer uses the current command-line spelling.
    replacements = {
        "--flash_mode": "--flash-mode",
        "--flash_freq": "--flash-freq",
        "--flash_size": "--flash-size",
    }
    args = [replacements.get(arg, arg) for arg in args]

    # merge-bin inputs are address/file pairs after the flash options. Checking
    # them here turns a generic esptool exit code 2 into an actionable artifact
    # error and catches a stale .gitlab-ci.yml artifact filename immediately.
    image_paths = [
        args[index + 1]
        for index, arg in enumerate(args[:-1])
        if re.fullmatch(r"0x[0-9a-fA-F]+", arg)
    ]
    if not image_paths:
        raise RuntimeError(f"{flash_args_path}: no address/image pairs found")
    missing = [
        image for image in image_paths
        if not os.path.isfile(os.path.join(build_dir, image))
    ]
    if missing:
        raise RuntimeError(
            f"{flash_args_path} references missing release artifact(s): "
            f"{', '.join(missing)}"
        )
    return args


def collect_release_tags(current_tag: str, previous=None) -> list:
    """Download labels from the published config; git tags do not prove upload."""
    tags = (previous or {}).get("website", {}).get("releases", [])
    values = ([] if current_tag.startswith("device-core-v") else [current_tag]) + tags
    return list(dict.fromkeys(tag for tag in values
                              if isinstance(tag, str) and tag and not tag.startswith("device-core-v")))


def merge_board(board: str, target: str, tag: str, build_dir: str, layout="legacy", entries=None) -> str:
    """Merge a single board's firmware into binaries/ and return the bin name."""
    bin_name = bin_name_for(board, tag)
    print(f"[{board}] merging {build_dir} -> {bin_name}")
    merge_args = load_merge_args(build_dir)
    subprocess.run(
        ["esptool", "--chip", target, "merge-bin", "-o", bin_name, *merge_args],
        cwd=build_dir,
        check=True,
    )
    shutil.move(os.path.join(build_dir, bin_name), os.path.join(OUTPUT_DIR, bin_name))
    from firmware_metadata import publish_update
    entry = publish_update(board, target, tag, build_dir, OUTPUT_DIR, os.path.join(OUTPUT_DIR, bin_name), layout=layout)
    if entries is not None:
        entries.append(entry)
    return bin_name


def collect_images(cfg: dict, tag: str, dry_run: bool, layout="legacy", entries=None) -> dict:
    """Return board -> (target, bin_name) for boards with a (real/predicted) bin.

    Real run: merge each board's artifacts. Dry run: predict bin names without
    touching IDF/esptool, and skip optional boards (best-effort, may be absent
    from a real release) so the snapshot mirrors a conservative release.
    """
    images = {}
    for board, meta in cfg["boards"].items():
        target = meta["target"]
        optional = meta.get("optional", False)
        if dry_run:
            if optional:
                print(f"[{board}] optional -> flashable=false in dry-run")
                continue
            images[board] = (target, bin_name_for(board, tag))
            continue
        build_dir = find_build_dir(board)
        if not build_dir:
            if optional:
                print(f"[{board}] no build artifacts found (optional) -> skipping")
                continue
            raise RuntimeError(
                f"no build dir with flash_args for {board}; "
                f"is the firmware:[...,{board}] job a dependency?"
            )
        images[board] = (target, merge_board(board, target, tag, build_dir, layout, entries))
    return images


def collect_models(source_commit=None) -> list:
    """Scan every models/ subfolder for valid model sidecars -> [[website.models]].

    Discovery is driven by the .json description files (not the model binaries),
    so a model in any subfolder is picked up as soon as it has a valid sidecar.
    A JSON that is not a model description (lacks the sentinel keys) is ignored;
    one that looks like a model but misses a required key, or whose authored
    sizeBytes does not match its sibling binary, is an error. The binary is the
    same-stem sibling of any extension. slug/file/path/downloadUrl come from
    the file location so the listing can never drift from what ships.
    """
    source_commit = source_commit or subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, text=True).strip()
    records = []
    for sidecar in glob.glob(os.path.join(MODELS_DIR, "**", "*.json"), recursive=True):
        with open(sidecar, encoding="utf-8") as f:
            meta = json.load(f)
        if not isinstance(meta, dict) or not MODEL_SENTINEL_KEYS.issubset(meta):
            continue  # not a model description sidecar
        rel = os.path.relpath(sidecar, MODELS_DIR)
        missing = [key for key in MODEL_REQUIRED_KEYS if key not in meta]
        if missing:
            raise RuntimeError(f"{rel}: model sidecar missing required keys: {', '.join(missing)}")
        # The model binary is the sibling file with the same stem and any other
        # extension (.espdl today, possibly other formats later) -- never assume
        # a fixed suffix.
        stem = os.path.splitext(os.path.basename(sidecar))[0]
        sidecar_dir = os.path.dirname(sidecar)
        binaries = [
            entry for entry in os.listdir(sidecar_dir)
            if entry != os.path.basename(sidecar) and os.path.splitext(entry)[0] == stem
        ]
        if not binaries:
            raise RuntimeError(f"{rel}: model sidecar has no sibling model file")
        if len(binaries) > 1:
            raise RuntimeError(f"{rel}: ambiguous model files for sidecar: {', '.join(sorted(binaries))}")
        model_path = os.path.join(sidecar_dir, binaries[0])
        actual_size = os.path.getsize(model_path)
        if meta["sizeBytes"] != actual_size:
            raise RuntimeError(
                f"{rel}: sizeBytes {meta['sizeBytes']} does not match file ({actual_size} bytes)"
            )
        model_rel = os.path.relpath(model_path, MODELS_DIR)
        rec = dict(meta)
        rec["slug"] = "-".join(os.path.dirname(model_rel).split(os.sep))
        rec["file"] = os.path.basename(model_path)
        rec["path"] = "models/" + model_rel.replace(os.sep, "/")
        rec["downloadUrl"] = MODEL_DOWNLOAD_BASE_URL + source_commit + "/" + rec["path"]
        records.append(rec)
    records.sort(key=lambda r: r["slug"])
    return records


def build_website(cfg: dict, images: dict, tag: str) -> dict:
    """Assemble the [website] region for boards enabled for the hardware matrix.

    Capability data comes from board_model.board_info (single source of derived
    truth). flashable/bin link each board to the standard launchpad region.
    """
    boards = []
    for board, meta in cfg["boards"].items():
        if not meta.get("website", True):
            continue
        target = meta["target"]
        info = board_info(board, target)
        flashable = board in images
        rec = {
            "slug": _slugify(info["name"]),
            "name": info["name"],
            "board": board,
            "chip": info["chip"],
            "target": target,
            "image": info.get("website_image", info["image"]),
            # canonical (en) doc URL; website swaps /en/ <-> /zh_CN/ per locale
            "docsUrl": info["url"],
            "status": "supported",
            "modules": list(info["modules"]),
            "micropython": list(info["port_features"]),
            "vision": [
                VISION_TOKEN.get(label, _slugify(label))
                for label in info["imlib_features"]
            ],
            "flashable": flashable,
        }
        if flashable:
            rec["bin"] = images[board][1]
        boards.append(rec)
    boards.sort(key=lambda b: b["slug"])
    return {
        "schemaVersion": WEBSITE_SCHEMA_VERSION,
        "release": tag,
        # Every published tag, newest first. Combined with binaryNameTemplate
        # and firmware_images_url, the site builds any version's URL itself.
        "releases": collect_release_tags(tag),
        "binaryNameTemplate": bin_name_template(),
        "boards": boards,
        # Bundled .espdl models shipped under models/. Not compiled into any
        # firmware (loaded at runtime via espdl); listed so the site can show
        # what ships with the release. Read straight from the .json sidecars.
        "models": collect_models(),
    }


def build_toml(cfg: dict, images: dict, tag: str) -> dict:
    """Assemble the full config.toml object (standard region + [website])."""
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
        # Ignored by esp-launchpad; consumed by the ESP-VISION marketing site.
        "website": build_website(cfg, images, tag),
    }


def unquote_keys(text: str) -> str:
    """Strip the quotes rtoml adds around dotted launchpad keys."""
    return re.sub(r'"((image|developKits)\.[\w-]+)"', r"\1", text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="release tag, e.g. v1.0.0")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="config only: skip merge-bin, predict bin names (no IDF/esptool)",
    )
    parser.add_argument("--layout", choices=("legacy", "single-bin"), default="legacy")
    parser.add_argument("--previous-config", help="previous published config bytes; default: fetch official config")
    parser.add_argument("--pipeline-id", type=int, default=int(os.environ.get("CI_PIPELINE_ID", "0")))
    parser.add_argument("--allow-missing-mcp", action="store_true", help="record mcp:null rather than referencing a stale pack")
    args = parser.parse_args()
    if not args.tag:
        parser.error("tag must be non-empty")

    with open(BOARDS_FILE) as f:
        cfg = yaml.safe_load(f)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    entries = []
    images = collect_images(cfg, args.tag, args.dry_run, args.layout, entries)
    if not images and not args.dry_run:
        raise RuntimeError("no boards merged; refusing to write an empty config.toml")

    toml_obj = build_toml(cfg, images, args.tag)
    if not args.dry_run:
        from pathlib import Path
        from publication import ROOT_URL, fetch_public, write_publication
        previous = Path(args.previous_config).read_bytes() if args.previous_config else fetch_public(ROOT_URL + "config.toml")
        source_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, text=True).strip()
        write_publication(OUTPUT_DIR, toml_obj, entries, args.tag, source_commit, args.pipeline_id,
                          previous, layout=args.layout, allow_missing_mcp=args.allow_missing_mcp)
    text = unquote_keys(rtoml.dumps(toml_obj))
    out = os.path.join(OUTPUT_DIR, "config.toml")
    with open(out, "w") as f:
        f.write(text)

    print(f"\nwrote {out}:\n{text}")


if __name__ == "__main__":
    main()
