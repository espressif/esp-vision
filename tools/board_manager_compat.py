# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Apply ESP-VISION compatibility fixes to generated Board Manager C only.

Board Manager 0.6.1 omits psram_dma_direct before IDF 6.1, including the
5.5.5 and 6.0.2 backports. Never patch a managed component or install cache.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

import yaml


def supports_psram_dma_direct(version: tuple[int, int, int]) -> bool:
    major, minor, patch = version
    return (major == 5 and minor == 5 and patch >= 5) or (major == 6 and minor == 0 and patch >= 2) or version >= (6, 1, 0)


def read_idf_version(idf_path: Path) -> tuple[int, int, int]:
    # ESP_IDF_VERSION in this project's environment is often only major.minor.
    # Read the actual compiler header so backport patch boundaries are exact.
    header = (idf_path / "components/esp_common/include/esp_idf_version.h").read_text(encoding="utf-8")
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^\s*#define\s+ESP_IDF_VERSION_{part}\s+(\d+)\s*$", header, re.MULTILINE)
        if match is None:
            raise ValueError(f"IDF version header has no numeric {part}")
        parts.append(int(match[1]))
    return tuple(parts)


def requested_spi_flags(board_dirs: list[Path]) -> dict[str, bool]:
    """Respect explicit board values; local amendments override base values."""
    flags = {}
    for board_dir in dict.fromkeys(board_dirs):
        path = board_dir / "board_devices.yaml"
        if not path.is_file():
            continue
        value = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for device in value.get("devices", []):
            config = device.get("config", {}).get("io_spi_config", {}).get("flags", {})
            if "psram_dma_direct" not in config:
                continue
            enabled = config["psram_dma_direct"]
            if not isinstance(enabled, bool) or not isinstance(device.get("name"), str):
                raise ValueError(f"Invalid explicit SPI DMA flag in {path}")
            flags[device["name"]] = enabled
    return flags


_LCD_CONFIG = re.compile(r"^\s*(?:(?:static\s+const|const\s+static)\s+)?dev_display_lcd_config_t\s+\w+\s*=\s*\{.*?^\};", re.MULTILINE | re.DOTALL)
_IO_FLAGS = re.compile(r"(\.io_spi_config\s*=\s*\{[^{}]*\.flags\s*=\s*\{)([^{}]*)(\})")
_DIRECT_FLAG = re.compile(r"(?P<indent>^[ \t]*)\.psram_dma_direct\s*=\s*(?:true|false|0|1)\s*,", re.MULTILINE)


def patch_generated_spi(source: str, requested: dict[str, bool], version: tuple[int, int, int]) -> tuple[str, list[str]]:
    if not supports_psram_dma_direct(version) or not requested:
        return source, []
    changed = []
    found = set()

    def patch_config(match: re.Match) -> str:
        config = match[0]
        name = re.search(r'\.name\s*=\s*("[^"\\]*")', config)
        if name is None:
            raise ValueError("Generated LCD configuration has no unescaped name")
        name = json.loads(name[1])
        if name not in requested or not re.search(r'\.sub_type\s*=\s*"spi"', config):
            return config
        if name in found:
            raise ValueError(f"Duplicate generated SPI LCD {name}")
        found.add(name)
        io = _IO_FLAGS.search(config)
        if io is None:
            raise ValueError(f"Generated SPI LCD {name} has no recognizable IO flags initializer")
        body = io[2]
        desired = "true" if requested[name] else "false"
        existing = list(_DIRECT_FLAG.finditer(body))
        if len(existing) > 1:
            raise ValueError(f"Duplicate generated psram_dma_direct for {name}")
        if existing:
            body = _DIRECT_FLAG.sub(lambda item: f"{item['indent']}.psram_dma_direct = {desired},", body)
        else:
            indent = re.search(r"\n([ \t]+)\.\w+\s*=", body)
            if indent is None:
                raise ValueError(f"Generated SPI LCD {name} has no recognizable IO flag indentation")
            end = len(body.rstrip())
            body = body[:end] + f"\n{indent[1]}.psram_dma_direct = {desired}," + body[end:]
        patched = config[:io.start(2)] + body + config[io.end(2):]
        if patched != config:
            changed.append(name)
        return patched

    patched = _LCD_CONFIG.sub(patch_config, source)
    # A changed generator layout must fail visibly rather than silently restore
    # dynamic internal-DMA allocation under Wi-Fi load.
    if requested.keys() - found:
        raise ValueError(f"Explicit SPI DMA device absent from generated SPI LCD configs: {sorted(requested.keys() - found)}")
    return patched, changed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--board-dir", type=Path, action="append", required=True)
    parser.add_argument("--idf-path", type=Path, required=True)
    args = parser.parse_args()
    version = read_idf_version(args.idf_path)
    requested = requested_spi_flags(args.board_dir)
    source = args.generated.read_text(encoding="utf-8")
    patched, changed = patch_generated_spi(source, requested, version)
    if patched != source:
        args.generated.write_text(patched, encoding="utf-8")
    print(json.dumps({"idf": ".".join(map(str, version)), "psram_dma_direct_supported": supports_psram_dma_direct(version), "patched_devices": changed}))


if __name__ == "__main__":
    main()
