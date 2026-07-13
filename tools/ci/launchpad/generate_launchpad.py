#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Merge per-board ESP-VISION firmware and emit an esp-launchpad config.toml.

In the tag-gated ``pack_launchpad`` CI job this reuses the artifacts produced by
the ``firmware`` build matrix (``build/<BOARD>/idf*/``) rather than rebuilding,
merges each board into a single flashable image whose name is suffixed with the
release tag, and writes ``binaries/config.toml``.

The config.toml carries two regions in one file:

  * the standard esp-launchpad schema (``firmware_images_url`` / ``image.*`` /
    ``developKits.*``) — drives the online flasher, lists only boards that were
    actually merged this release. Still openable by espressif.github.io/esp-launchpad.
  * a ``[website]`` table (ignored by esp-launchpad) — the ESP-VISION marketing
    site's board matrix, versioned by ``schemaVersion`` (semver; consumers pin
    the major). Lists *every* board in boards.yml with its capability
    data (from docs/board_model.board_info, the single source of derived truth),
    plus ``flashable`` / ``bin`` linking each board to the standard region. It
    also carries ``[[website.models]]``: the bundled ``.espdl`` models shipped
    under ``models/``, read straight from each model's same-named ``.json``
    sidecar, with size/architecture/task/download URLs so the site can list and
    link them. Finally it carries ``releases`` (every published tag, newest first) plus
    ``binaryNameTemplate``: because every image is named
    ``esp-vision-<board>-<tag>.bin``, the site can offer *all* historical
    versions by templating the URL itself (``firmware_images_url`` + filled
    template), without the config listing each version's bins explicitly.

Because every published binary carries the tag in its filename and the uploader
copies additively (``aws s3 cp --recursive``, no ``--delete``), older releases
remain downloadable; only ``config.toml`` is overwritten to point launchpad at
the newest tag.

Usage:
    generate_launchpad.py <tag>              # real release: merge bins + config
    generate_launchpad.py --dry-run <tag>    # config only (no IDF / no bins)

``--dry-run`` skips merge_bin entirely and predicts bin names from the
deterministic ``esp-vision-<BOARD>-<tag>.bin`` pattern. It needs neither build
artifacts nor esptool, so it runs locally and in non-release CI. Its output is
used to (a) seed the website's committed config.toml snapshot for build-time
SSG, and (b) catch board-config drift early. Optional boards (best-effort, may
be absent from a real release) are marked ``flashable = false`` in dry-run.
"""

import argparse
import glob
import json
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

# docs/ holds board_model.py + targets.py: the single source of derived board
# capability data, shared with the documentation board-support tables.
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "docs"))

from board_model import board_info  # noqa: E402

# Bundled .espdl models live here, each with a same-named .json metadata sidecar.
MODELS_DIR = os.path.join(REPO_ROOT, "models")
MODEL_DOWNLOAD_BASE_URL = "https://raw.githubusercontent.com/espressif/esp-vision/master/"

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


def collect_release_tags(current_tag: str) -> list:
    """Every published release tag, newest first.

    Releases are git tags (the pipeline is tag-gated and uploads are additive,
    so each tag's bins stay downloadable). Sorted by tag creation date so the
    order is stable regardless of the tag naming scheme. ``current_tag`` is
    always included and placed first: at release time it is the tag being built,
    and in --dry-run it seeds the snapshot even though it is not a real tag.
    """
    try:
        out = subprocess.run(
            ["git", "tag", "--sort=-creatordate"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        tags = [line.strip() for line in out.splitlines() if line.strip()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        tags = []
    if current_tag in tags:
        tags.remove(current_tag)
    return [current_tag] + tags


def merge_board(board: str, target: str, tag: str, build_dir: str) -> str:
    """Merge a single board's firmware into binaries/ and return the bin name."""
    bin_name = bin_name_for(board, tag)
    print(f"[{board}] merging {build_dir} -> {bin_name}")
    subprocess.run(
        ["esptool.py", "--chip", target, "merge_bin", "-o", bin_name, "@flash_args"],
        cwd=build_dir,
        check=True,
    )
    shutil.move(os.path.join(build_dir, bin_name), os.path.join(OUTPUT_DIR, bin_name))
    return bin_name


def collect_images(cfg: dict, tag: str, dry_run: bool) -> dict:
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
        images[board] = (target, merge_board(board, target, tag, build_dir))
    return images


def collect_models() -> list:
    """Scan every models/ subfolder for valid model sidecars -> [[website.models]].

    Discovery is driven by the .json description files (not the model binaries),
    so a model in any subfolder is picked up as soon as it has a valid sidecar.
    A JSON that is not a model description (lacks the sentinel keys) is ignored;
    one that looks like a model but misses a required key, or whose authored
    sizeBytes does not match its sibling binary, is an error. The binary is the
    same-stem sibling of any extension. slug/file/path/downloadUrl come from
    the file location so the listing can never drift from what ships.
    """
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
        rec["downloadUrl"] = MODEL_DOWNLOAD_BASE_URL + rec["path"]
        records.append(rec)
    records.sort(key=lambda r: r["slug"])
    return records


def build_website(cfg: dict, images: dict, tag: str) -> dict:
    """Assemble the [website] region: every board + capabilities + flash link.

    Capability data comes from board_model.board_info (single source of derived
    truth). flashable/bin link each board to the standard launchpad region.
    """
    boards = []
    for board, meta in cfg["boards"].items():
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
        help="config only: skip merge_bin, predict bin names (no IDF/esptool)",
    )
    args = parser.parse_args()
    if not args.tag:
        parser.error("tag must be non-empty")

    with open(BOARDS_FILE) as f:
        cfg = yaml.safe_load(f)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    images = collect_images(cfg, args.tag, args.dry_run)
    if not images and not args.dry_run:
        raise RuntimeError("no boards merged; refusing to write an empty config.toml")

    toml_obj = build_toml(cfg, images, args.tag)
    text = unquote_keys(rtoml.dumps(toml_obj))
    out = os.path.join(OUTPUT_DIR, "config.toml")
    with open(out, "w") as f:
        f.write(text)

    print(f"\nwrote {out}:\n{text}")


if __name__ == "__main__":
    main()
