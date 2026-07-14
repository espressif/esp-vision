# Changelog

All notable changes to ESP-VISION are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/); each released version corresponds to a git tag. Unreleased changes accumulate at the top and are folded into the next tag at release time.

## [Unreleased]

### Added

- Added ESP-IDF release/v6.1 support for all supported boards, including `ESP32_S31_KORVO`.
- Replaced the root `CLAUDE.md` with `AGENTS.md` to provide shared project guidance for AI coding agents.
- Added quick-access links to the ESP-VISION website and Web IDE in the English and Chinese introduction pages, including guidance to the website's MCP setup resources.
- Added a low-brightness status blink on the `ESP32_P4X_VISION` board's GPIO9 WS2812 in the default first-boot `main.py`, with the NeoPixel driver frozen into that board's firmware.

### Fixed

- Fixed sequential LCD scripts by transferring teardown ownership of the shared board display to the newest successfully initialized `display.Display` object, so delayed finalization of an older wrapper cannot deinitialize the LCD reused by the current script.

### Removed

- Removed the ESP-IDF master component manifest and build fallback; firmware builds now support only release/v5.5, release/v6.0, and release/v6.1.

## [2026.06.27]

### Added

- Added `sensor.VGA` framesize support for camera output on supported P4 and S31 boards.
- Added an ESPDet Pico hardhat model and English/Chinese documentation for training ESPDet Pico models.
- Added a generic `espdl.Model` runner that exposes ESP-DL input/output tensor metadata and raw output bytes for Python-side post-processing, plus `example/03-Machine-Learning/00-ESP-DL/espdet_pico_python.py` as an ESPDet Pico reference using Python decode and NMS.

### Fixed

- ESP32_S31_KORVO camera startup now drives XCLK and SCCB I2C from board code before `esp_video` initialization, applies an OV3660 soft reset, and retries the DVP stream/init path internally before Python sees an error.
- MCP API knowledge pack generation for the documentation sidecar, published at release time under `mcp/latest.json` and `mcp/mcp-pack-<tag>.json`, with CI schema validation.
- `config.toml` `[[website.models]]` entries now include `downloadUrl`, derived from each model's repository path and pointing to the public GitHub `master` raw URL for website model downloads.

### Fixed

- MCP pack MicroPython extraction now excludes ESP8266-only `esp` APIs from the ESP32 target set.
- MCP pack RST parsing now ignores Sphinx directive options and joins wrapped signatures, avoiding invalid entries such as `:noindex:` or orphaned continuation lines.
- Release publishing now copies `mcp/latest.json` only after pack generation and validation both succeed.

## [2026.06.22]

### Added

- TensorFlow Lite Micro support via a new `tflite` module (`tflite.Model`) on ESP32-P4/S3/S31: run `.tflite` models with ulab ndarray input/output, exposed quantization metadata, and an optional post-processing callback. Ships two bundled models under `models/tflite/` (`person_detect`, `sine`) and examples under `example/03-Machine-Learning/01-TFLite/` (`person_detection.py`, `sine.py`).
- New board `ESP32_P4X_VISION` (ESP32-P4, 16 MiB flash layout): SC101IOT DVP camera backend, SDMMC slot 0, and flash MSC. Display is a board-level placeholder until the LCD hardware is enabled.
- New example `example/.../01-Cloud-AI/openai_compatible_vision.py`: send camera frames to an OpenAI-compatible vision API from the board.
- esp-launchpad `config.toml` now lists the bundled AI models under `[[website.models]]`. Each model lives in any `models/` subfolder as a binary plus a same-named `.json` sidecar (`name`, `architecture`, runtime `api`, `task`, `input`, `inputFormat`, `dataset`, `labels`, `sizeBytes`, and optional `description`/`datasetUrl`); the generator scans all subfolders for sidecars, so the website can list what ships with a release. Adding a model is just dropping in both files; the size is cross-checked against the binary.
- `config.toml`'s `[website]` table now carries `releases` (every published firmware tag, newest first) and `binaryNameTemplate` (`esp-vision-{board}-{tag}.bin`). Because the naming is consistent, the website builds any historical version's firmware download URL itself from `firmware_images_url` + the template, without listing each version's bins.

### Changed

- `config.toml` `[website].schemaVersion` is now a semver string (was the integer `1`); consumers pin on the major component.

## [2026.06.17]

Initial release.
