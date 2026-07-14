# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""ESP-VISION idf.py project extension.

This file lets developers run ESP-IDF's native idf.py commands from the
ESP-VISION repository root while the actual CMake project remains the
generated MicroPython ESP32 port under build/micropython/.
"""

from __future__ import annotations

import filecmp
import os
import re
import shutil
import subprocess
import tarfile
from typing import Any

import click

from idf_py_actions.errors import FatalError


DEFAULT_BOARD = "ESP32_P4X_EYE"
MP_BASE_REF = "v1.28.0"
MP_BASE_COMMIT = "e0e9fbb17ed6fd06bb76e266ae554784c9c80804"
ESP_VISION_ACTIONS = {
    "all",
    "clean",
    "erase-flash",
    "flash",
    "fullclean",
    "menuconfig",
    "monitor",
    "reconfigure",
    "size",
}
PASSTHROUGH_ACTIONS = {"docs", "help", "python-clean"}
SUPPORTED_ACTIONS_HELP = (
    "build, flash, monitor, menuconfig, clean, fullclean, reconfigure, size, "
    "erase-flash"
)


def action_extensions(base_actions: dict[str, Any], project_path: str = os.getcwd()) -> dict[str, Any]:
    root = os.path.realpath(project_path)
    boards_dir = os.path.join(root, "boards")
    mp_repo = os.path.join(root, "lib", "micropython")
    mp_overlay = os.path.join(root, "overlay", "micropython")
    mp_component_yml = os.path.join(root, "overlay", "component_yml")
    components_dir = os.path.join(root, "components")

    def supported_boards() -> list[str]:
        if not os.path.isdir(boards_dir):
            return []

        boards = []
        for name in sorted(os.listdir(boards_dir)):
            # boards/<name>/port/ holds the MicroPython-port files and marks a
            # buildable board; it is projected onto the port at prepare time.
            port_path = os.path.join(boards_dir, name, "port")
            if os.path.isdir(port_path):
                boards.append(name)
        return boards

    board_choices = supported_boards() or [DEFAULT_BOARD]

    def idf_version() -> str:
        version = os.environ.get("ESP_IDF_VERSION")
        if version:
            return version

        version_cmake = os.path.join(os.environ.get("IDF_PATH", ""), "tools", "cmake", "version.cmake")
        values: dict[str, str] = {}
        if os.path.exists(version_cmake):
            pattern = re.compile(r"^\s*set\s*\(\s*IDF_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)")
            with open(version_cmake, encoding="utf-8") as f:
                for line in f:
                    match = pattern.match(line)
                    if match:
                        values[match.group(1)] = match.group(2)

        if "MAJOR" in values and "MINOR" in values:
            version = f"{values['MAJOR']}.{values['MINOR']}"
            os.environ["ESP_IDF_VERSION"] = version
            return version

        raise FatalError("ESP_IDF_VERSION is not set; source the ESP-IDF export script before running idf.py")

    def ensure_idf_version_env() -> str:
        version = idf_version()
        if not os.environ.get("IDF_VERSION"):
            os.environ["IDF_VERSION"] = version
        return version

    def validate_board(board: str) -> str:
        if board not in board_choices:
            raise FatalError(
                "Unsupported BOARD '{}'. Supported boards: {}".format(board, ", ".join(board_choices))
            )

        required_paths = [
            os.path.join(boards_dir, board, "manifest.py"),
            os.path.join(boards_dir, board, "port", "mpconfigboard.cmake"),
        ]
        for path in required_paths:
            if not os.path.exists(path):
                raise FatalError(f"Board {board} is missing required file: {path}")

        return board

    def build_dir_for(board: str) -> str:
        return os.path.join(root, "build", board, f"idf{idf_version()}")

    def mp_build_dir() -> str:
        return os.path.join(root, "build", "micropython", f"idf{idf_version()}", "micropython")

    def mp_port_dir() -> str:
        return os.path.join(mp_build_dir(), "ports", "esp32")

    def select_component_manifest() -> str:
        version = ensure_idf_version_env()
        candidates = [version]

        match = re.search(r"(\d+)\.(\d+)", version)
        if match:
            major_minor = f"{match.group(1)}.{match.group(2)}"
            if major_minor not in candidates:
                candidates.append(major_minor)

        for candidate in candidates:
            path = os.path.join(mp_component_yml, f"release{candidate}", "idf_component.yml")
            if os.path.exists(path):
                return path

        raise FatalError(
            f"Unsupported ESP-IDF version {version}; supported releases are v5.5, v6.0, and v6.1"
        )

    def selected_idf_overlay_name() -> str:
        manifest = select_component_manifest()
        return os.path.basename(os.path.dirname(manifest))

    def validate_board_idf_overlay(board: str, overlay_name: str) -> None:
        if board == "ESP32_S31_KORVO" and overlay_name != "release6.1":
            raise FatalError(
                "ESP32_S31_KORVO is supported only with ESP-IDF "
                f"release/v6.1 (current overlay: {overlay_name})"
            )

    def git_output(repo: str, *args: str) -> str:
        try:
            return subprocess.check_output(
                ["git", "-C", repo, *args],
                encoding="utf-8",
                stderr=subprocess.STDOUT,
            )
        except subprocess.CalledProcessError as e:
            raise FatalError(e.output.strip() or f"git {' '.join(args)} failed in {repo}")

    def git_run(repo: str, *args: str) -> None:
        try:
            subprocess.run(
                ["git", "-C", repo, *args],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                encoding="utf-8",
            )
        except subprocess.CalledProcessError as e:
            raise FatalError(e.stdout.strip() or f"git {' '.join(args)} failed in {repo}")

    def export_git_tree(repo: str, dst: str) -> None:
        os.makedirs(dst, exist_ok=True)
        archive_path = os.path.join(dst, ".esp_vision_src.tar")
        git_run(repo, "archive", "--format=tar", "-o", archive_path, "HEAD")
        try:
            with tarfile.open(archive_path, mode="r:") as archive:
                archive.extractall(dst)
        except tarfile.TarError as e:
            raise FatalError(f"failed to extract git archive from {repo}: {e}") from e
        finally:
            if os.path.exists(archive_path):
                os.remove(archive_path)

    def submodule_paths() -> list[str]:
        # Match the Makefile path: foreach only visits initialized submodules,
        # while status --recursive also lists missing ones in shallow CI checkouts.
        output = git_output(
            mp_repo,
            "submodule",
            "foreach",
            "--recursive",
            "--quiet",
            'printf "%s\\n" "$displaypath"',
        )
        paths = []
        for line in output.splitlines():
            path = line.strip()
            if path:
                paths.append(path)
        return paths

    def prepare_micropython_tree() -> str:
        if not os.path.isdir(mp_repo):
            raise FatalError("lib/micropython is missing; initialize submodules before building")

        head = git_output(mp_repo, "rev-parse", "HEAD").strip()

        if head != MP_BASE_COMMIT:
            raise FatalError(f"lib/micropython must be checked out at {MP_BASE_REF} ({MP_BASE_COMMIT})")

        mp_build = mp_build_dir()
        if os.path.exists(mp_build):
            shutil.rmtree(mp_build)

        export_git_tree(mp_repo, mp_build)
        for submodule_path in submodule_paths():
            submodule_src = os.path.join(mp_repo, submodule_path)
            if not os.path.isdir(submodule_src):
                continue

            submodule_dst = os.path.join(mp_build, submodule_path)
            if os.path.exists(submodule_dst):
                shutil.rmtree(submodule_dst)
            export_git_tree(submodule_src, submodule_dst)

        if os.path.isdir(mp_overlay):
            shutil.copytree(mp_overlay, mp_build, dirs_exist_ok=True)

        # Project each board's MicroPython-port files (boards/<board>/port/)
        # onto the prepared port. boards/<board>/ is the single authoring
        # location; the matching ports/esp32/boards/<board>/ is generated here
        # rather than tracked under overlay/.
        for name in sorted(os.listdir(boards_dir)):
            port_dir = os.path.join(boards_dir, name, "port")
            if not os.path.isdir(port_dir):
                continue
            dst = os.path.join(mp_port_dir(), "boards", name)
            if os.path.exists(dst):
                shutil.rmtree(dst)
            shutil.copytree(port_dir, dst)

        src = select_component_manifest()
        dst = os.path.join(mp_port_dir(), "main", "idf_component.yml")
        if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
            shutil.copy2(src, dst)

        return os.path.relpath(src, root)

    def parse_defines(defines: list[str]) -> dict[str, str]:
        parsed: dict[str, str] = {}
        for define in defines:
            if "=" in define:
                key, value = define.split("=", 1)
                parsed[key] = value
        return parsed

    def set_define(args: Any, key: str, value: str) -> None:
        args.define_cache_entry = [
            define for define in args.define_cache_entry if not define.startswith(f"{key}=")
        ]
        args.define_cache_entry.append(f"{key}={value}")

    def require_or_set_define(args: Any, key: str, value: str) -> None:
        existing = parse_defines(args.define_cache_entry).get(key)
        if existing is not None and os.path.realpath(existing) != os.path.realpath(value):
            raise FatalError(f"-D{key}={existing} conflicts with ESP-VISION value {value}")
        set_define(args, key, value)

    def set_extra_component_dirs(args: Any) -> None:
        existing = parse_defines(args.define_cache_entry).get("EXTRA_COMPONENT_DIRS", "")
        paths = [path for path in existing.split(";") if path]
        if not any(os.path.realpath(path) == components_dir for path in paths):
            paths.append(components_dir)
        set_define(args, "EXTRA_COMPONENT_DIRS", ";".join(paths))

    def configure_idf_args(args: Any, board: str, overlay_name: str) -> None:
        original_project_dir = os.path.realpath(args.project_dir)
        original_default_build_dir = os.path.realpath(os.path.join(original_project_dir, "build"))

        args.project_dir = mp_port_dir()
        if args.build_dir is None or os.path.realpath(args.build_dir) == original_default_build_dir:
            args.build_dir = build_dir_for(board)
        else:
            args.build_dir = os.path.realpath(args.build_dir)

        require_or_set_define(args, "MICROPY_BOARD", board)
        require_or_set_define(args, "ESP_VISION_IDF_OVERLAY", overlay_name)
        require_or_set_define(args, "USER_C_MODULES", os.path.join(root, "micropython.cmake"))
        require_or_set_define(
            args,
            "MICROPY_FROZEN_MANIFEST",
            os.path.join(boards_dir, board, "manifest.py"),
        )
        set_extra_component_dirs(args)

    def should_prepare(tasks: list[Any], dry_run: bool) -> bool:
        if dry_run:
            return False

        no_prepare_actions = {"clean", "fullclean"}
        return any(task.name not in no_prepare_actions for task in tasks)

    def esp_vision_global_callback(ctx: Any, args: Any, tasks: list[Any]) -> None:
        task_names = {task.name for task in tasks}
        unsupported = task_names - ESP_VISION_ACTIONS - PASSTHROUGH_ACTIONS
        if unsupported:
            raise FatalError(
                "ESP-VISION idf.py supports only: {}. Unsupported action(s): {}".format(
                    SUPPORTED_ACTIONS_HELP,
                    ", ".join(sorted(unsupported)),
                )
            )

        if not task_names & ESP_VISION_ACTIONS:
            return

        if not args.board:
            raise FatalError("ESP-VISION requires --board for this action, for example: idf.py --board ESP32_P4X_EYE build")

        board = validate_board(args.board)
        ensure_idf_version_env()
        overlay_name = selected_idf_overlay_name()
        validate_board_idf_overlay(board, overlay_name)
        configure_idf_args(args, board, overlay_name)

        if should_prepare(tasks, args.dry_run):
            selected = prepare_micropython_tree()
            print(
                "[prepare-micropython] selected component manifest: "
                f"{selected} (ESP-IDF {idf_version()})"
            )

    return {
        "version": "1",
        "global_options": [
            {
                "names": ["--board"],
                "help": "ESP-VISION board package to build.",
                "scope": "shared",
                "type": click.Choice(board_choices),
                "default": None,
            },
        ],
        "global_action_callbacks": [esp_vision_global_callback],
        "actions": {},
    }
