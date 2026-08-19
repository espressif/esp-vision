# Changelog

All notable changes to ESP-VISION are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/); each released version corresponds to a git tag. Unreleased changes accumulate at the top and are folded into the next tag at release time.

## [Unreleased]

### Added

- Added the shared ESP-DL PicoDet pedestrian model, an `espdl.PedestrianDetect` wrapper with model-specific preprocessing and post-processing, Python stubs, and a camera example for ESP32-P4/S31/S3 boards.
- Added a button-triggered PP-OCRv6 pipeline with separate INT8 text detection and recognition stages, Python detection/CTC post-processing, and generic BGR preprocessing through `espdl.Model`.
- Added ESP32-S3 USB-OTG CDC automatic download-mode entry using the esptool USB-Serial/JTAG DTR/RTS sequence, controlled per board from `mpconfigboard.h` and handled outside the MicroPython VM. The existing EV-MUX CDC stack now starts before camera initialization, filesystem recovery, and `boot.py`; its transport task keeps the download path responsive while the VM is busy without introducing a second USB driver or descriptor owner.
- Added OpenMV v5.0.0's MIT-licensed Edge Drawing Lines implementation and enabled `Image.find_line_segments()` on all supported boards.

### Changed

- Added periodic MicroPython event polling to long-running imlib drawing, filtering, feature-detection, QR, statistics, and template-matching loops.
- Synchronized the MicroPython Wi-Fi authentication constants with the native and remote Wi-Fi backends in ESP-IDF 6.0 and 6.1.
- Moved Flash and SD FAT filesystem ownership to an ESP-IDF storage manager, with native MicroPython VFS bridges at `/` and `/sdcard` and a shared raw Flash backend for the existing MSC LUN.
- Exposed SD cards as MSC LUN 1 on SD-capable boards while retaining Flash as LUN 0.
- Moved the ESP-VISION USB-Serial-JTAG interface selection to each board's `mpconfigboard.h`, explicitly enabled the USJ clock and internal PHY from the MicroPython driver, returned the ESP-IDF system console to its target defaults, removed redundant board sdkconfig overrides, and clarified transport reliability and observability documentation.
- Changed the ESP32-S3 automatic-download hand-off to defer the DTR/RTS-triggered reset out of the TinyUSB line-state callback and independently apply the MicroPython-proven USB persistence sequence before entering ROM USB-Serial/JTAG download mode.
- Documented esptool `--after watchdog-reset` for leaving an ESP32-S3 ROM loader entered through USB-Serial/JTAG.

### Fixed

- Delayed the EV-MUX transport task until TinyUSB initialization completes, guaranteed at least one RTOS tick between pumps, and configured the ESP32-S3 boards for a 1000 Hz FreeRTOS tick, preventing USB startup races and MicroPython task starvation.
- Fixed EV-MUX script execution to close the source file before running it, allowing a script to be overwritten immediately after it finishes.
- Fixed ESP32-P4X-EYE LCD updates under camera and inference memory pressure by sending the PSRAM framebuffer through SPI DMA directly.
- Fixed EV-MUX CDC startup on boards with a 100 Hz FreeRTOS tick by guaranteeing the higher-priority transport task delays for at least one tick instead of starving the MicroPython main task with `vTaskDelay(0)`; CDC hello discovery now waits for DTR-ready state, and USB serial descriptors encode only the initialized six-byte factory MAC.

### Removed

- Removed generic `vfs.VfsFat` support from ESP32 firmware now that board Flash and SD FAT volumes are owned by ESP-IDF; `vfs.VfsLfs2` remains available for independent block devices.

## [2026.07.28]

### Added

- Added Windows host support for repository-root `idf.py --board <BOARD> ...` firmware builds.
- Added an agent-oriented MCP knowledge guide that requires ESP-VISION code to target the supported MicroPython runtime, defaults generated applications to an all-in-one script, rejects unprovisioned `requirements.txt` dependencies, and provides a copyable USB-OTG CDC/USJ EV-MUX closed-loop test.

### Fixed

- Fixed tag release packaging after the application binary was renamed from `micropython.bin` to `esp-vision.bin`, updated the launchpad merge step to validate its input artifacts and use the current esptool command syntax, and deferred GitHub synchronization until the deploy stage succeeds.
- Silenced known deprecation and shadow warnings from the current `esp_video` and `esp-tflite-micro` managed component releases without disabling those warnings for ESP-VISION sources.
- Fixed clean firmware builds by providing the Git-derived firmware version to MicroPython's qstr preprocessing pass.
- Fixed ESP-IDF 6.1 firmware builds by synchronizing MicroPython's Wi-Fi authentication constants with the new unknown scan-result mode.
- EV-MUX protocol v3 with two routed streams: `user` (`user.rpc`, framed REPL, and `preview.frame`) and `debug` (`debug.rpc` and `log.idf`). The transport task parses independent USJ, CDC, and UART ingress streams while the VM is busy.
- EV-ATP host operations for discovery, stream routing, script upload/run, device control, transport statistics, filesystem/sensor inspection, and binary JPEG capture through `debug.rpc`.
- DTR-driven routing: both streams follow USB-OTG CDC while its port is open and fall back to the board's default sink when it closes; `route.bind` and `route.auto` support per-stream overrides.
- Preview flow control drops whole `preview.frame` frames from congested sinks and caps the USJ preview rate to one frame per 100 ms. `debug.info` scope `transport.stats` reports RX, parser, queue, timeout, and drop counters.
- `capabilities` reports the board USB product string and distinguishes CDC `present` (enumerated) from `ready` (DTR asserted).
- `hello`, `capabilities`, and device diagnostics expose a stable `firmware.id` plus the Git-derived ESP-IDF `PROJECT_VER`, allowing IDEs to distinguish release and development firmware builds without coupling protocol compatibility to a release string.
- VM-context RPCs queued for more than 5 seconds now fail with `VM_TIMEOUT` instead of occupying the queue indefinitely.
- EV-MUX replies return on the request ingress, including discovery requests received on a non-active USB sink.
- Single-CDC boards now follow CDC DTR instead of remaining on an unavailable USJ route.

### Changed

- `img.flush()` now sends binary JPEG data in `preview.frame` instead of the previous base64 `EVFRAME` text envelope.
- Python stdout/stderr, C stdio, ESP-IDF logs, and debug output are framed on their assigned EV-MUX channels while mux mode is enabled.
- The REPL banner and help text, runtime metadata, ESP-IDF application name, USB descriptors, and recovery/crash messages now use ESP-VISION branding.
- Architecture documentation now describes the two-stream routing, framing, RPC, and execution model.
- The top-level Make build interface; firmware builds now use the repository-root `idf.py --board <BOARD> ...` CMake interface exclusively.

## [2026.07.16]

### Added

- Added ESP-IDF release/v6.1 support for all supported boards, including `ESP32_S31_KORVO`.
- Replaced the root `CLAUDE.md` with `AGENTS.md` to provide shared project guidance for AI coding agents.
- Added quick-access links to the ESP-VISION website and Web IDE in the English and Chinese introduction pages, including guidance to the website's MCP setup resources.
- Added a low-brightness status blink on the `ESP32_P4X_VISION` board's GPIO9 WS2812 in the default first-boot `main.py`, with the NeoPixel driver frozen into that board's firmware.
- Added `ESP32_P4X_VISION` to the esp-launchpad release manifest and packaging job so release tags publish flashable firmware for the board.

### Fixed

- Fixed sequential LCD scripts by transferring teardown ownership of the shared board display to the newest successfully initialized `display.Display` object, so delayed finalization of an older wrapper cannot deinitialize the LCD reused by the current script.
- Fixed the board default LCD preview scripts and the `lcd_preview.py` example to release `sensor` and `display` resources on `Ctrl-C`, preventing retained LCD buffers from reducing the fast framebuffer memory available to later AprilTag runs.

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
