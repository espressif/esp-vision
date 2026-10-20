# Changelog

All notable changes to ESP-VISION are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/); each released version corresponds to a git tag. Unreleased changes accumulate at the top and are folded into the next tag at release time.

## [Unreleased]

### Fixed

- Reset browser-flashed devices through the active stub loader so its two-byte command status is decoded correctly; preserve automatic-reset failure diagnostics for runtime handoff.

- Separate the Chinese hardware-guide inline REPL limit literal from its following parenthesis so strict HTML/PDF documentation builds accept the new CI contract section.
- Retain a Core-captured LCD test-pattern JPEG alongside the camera JPEG in CI hardware artifacts and require its dimensions to match the live display.
- Preserve camera allocation, timeout and unexpected mode-switch errors as hardware-test failures; only explicit unsupported capabilities may be reported as unsupported. Keep identity checks compatible with firmware that does not advertise optional storage inspection.
- Keep SDK-only hardware-fixture regression tests independent of an installed Python interpreter; verify bounded source reconstruction in Node and execute the fixture through real Core hardware acceptance.
- Load hardware-test fixtures in RAM within the negotiated REPL input limit instead of requiring Flash file uploads for memory/camera/display checks. Report storage state separately and fail explicit file acceptance on unmounted volumes without formatting. Bind target-test startup and runtime checks to the selected board/build identity and require every selected CI check to pass. Ship the updated packaged hardware tools as Device Core 0.4.1 to avoid republishing changed 0.4.0 content.

### Changed

- Qualify ESP32-P4/S3 firmware and hardware CI on pinned ESP-IDF v5.5.6, retain release/v6.1 for ESP32-S31, and align release packaging dependencies with this board-specific matrix. Document the required IDF environments and possible build/runtime/Python API changes with other versions in English and Chinese.

## [2026.10.19]

This release introduces EV-MUX v4, prepares Device Core 0.4.0 browser flashing and release-manifest support, and adds NanoTrack, camera IPA/ISP controls and LCD touch input. Firmware release numbers, npm SDK versions and IDE versions are independent.

### Added

- Prepared Device Core 0.4.0 with immutable publication-manifest readers and verified application extraction from one complete firmware BIN, preserving layout checks, explicit installation, multi-file model groups and legacy feed compatibility. Immutable browser downloads reuse safe cache entries; model sources remain inside normalized commit-pinned paths.
- Added the Device Core `/browser` ROM adapter using pinned tasmota 9.4.2, centralizing P4/S31 browser compatibility and verified install/update workflows, with browser bundle and package isolation checks and bilingual API documentation.
- Added immutable per-version firmware manifests with hash-bound current/history pointers, actual board membership, commit-pinned website/model/MCP snapshots and compiler-derived Wi-Fi capabilities. Compatibility full/app/per-board images remain the default; explicitly enabled single-bin mode extracts qualified application slices from the complete BIN. All compiler contexts for repeated network-service sources must agree on Wi-Fi capability, and upload waits for the complete firmware/HIL/SDK/package qualification contract.
- Added the independently versioned [`@esp-vision/device-core`](https://www.npmjs.com/package/@esp-vision/device-core) host SDK in `host/device-core`. The SDK provides the grouped `createDevice()` API for connections, inspection, Wi-Fi, Python, files, capture, discovery and firmware updates, plus Node TCP/serial adapters, a management CLI and runnable package checks. The package uses Apache-2.0 and preserves historical MIT notices.
- Added EV-MUX v4 session ownership, leases, keepalive and bounded I/O across USB-Serial-JTAG, USB-OTG CDC, UART and TCP. Session and VM generations isolate REPL input/output, logs, preview and capture from stale owners and resets.
- Added background native/Hosted Wi-Fi with persisted provisioning and reconnect, a TCP endpoint and `_esp-vision._tcp.local` discovery. Network/TCP services default to enabled on supported Wi-Fi boards; credentials are still required. Board name, MAC suffix and firmware version form one canonical hostname shared by discovery and interface queries.
- Added reliable binary file upload for Python source, models and images through Core `files.write()`: raw blocks up to 16 KiB, CRC32, sequential offsets, length/SHA-256 verification and explicit commit/abort. The firmware holds one temporary native file open and cleans failed or stale transfers without replacing the existing target before verification.
- Added session-bound storage-writer leases and typed launcher boot/trial status for filesystem handoff and script lifecycle management.
- Added dedicated LCD screenshot capture through Core `capture({ source: 'screen' })`, backed by `debug.capture_screen`, with explicit not-ready/unsupported errors.
- Added `sensor.set_ipa()`, `get_ipa()`, `set_isp()`, `get_isp()` and `get_ipa_stats()` for runtime camera control and statistics, with stubs, documentation and examples. IPA/ISP control is enabled on `ESP32_P4X_EYE` and `ESP32_P4X_FUNCTION_EV_BOARD`; availability depends on the camera and board configuration.
- Added `espdl.NanoTrack`, NanoTrack V2 INT8 models and a BOOT-button-initialized single-object tracking example. The bundled model exports and target restrictions are documented under `models/nanotrack_v2/`.
- Added `lcd_touch.Touch` with `read()` and `deinit()`, with coordinate-reading and drawing examples for boards with configured touch hardware, including the P4 Function EV Board and S31 Mosaico.
- Added application-only firmware images and per-board/version metadata for verified ROM updates through Core: image hashes, partition bounds and chip revision limits. Updates require a compatible existing partition layout.
- Added explicit complete-image installation through Core `firmware.check/update({ mode: 'install' })` and the CLI over ROM-capable USJ/UART ports, without an EV-MUX connection. Release tooling qualifies merged bootloader/partition/application hashes, intersected silicon revision bounds and Flash capacity. Default updates retain their application-only behavior; historical images without qualified metadata cannot be installed through this flow.
- Added bilingual Device Core and EV-MUX documentation covering APIs, architecture, physical interfaces, ownership, Python/files, preview/capture and host integration, plus Core-based hardware-in-the-loop checks on the P4 Function EV Board.

### Changed

- **Breaking host-protocol change:** EV-MUX v3 hosts must move to a matching v4 host/SDK when installing this firmware. Device Core 0.2.0 uploads require firmware capability `file.write.binary`; older v4 firmware remains queryable but must be updated before uploading. Use Core for framed Python/file operations and session coordination.
- Unified host integration around Host applications → Device Core → EV-MUX → firmware. SDK and firmware are maintained together; IDE and Agent applications remain separate consumers. This firmware release does not ship the IDE 1.3.0 application or Agent plugins.
- Gated MSC/VFS handoff across live file operations. FAT file replacement retains an old-file backup for ordinary rename rollback; it is not a power-loss-atomic journal.
- Upgraded ESP-DL from 3.3.6 to 3.3.13, enabled dual-core NanoTrack inference on ESP32-P4 and optimized tracker preprocessing.
- Renamed board default credential macros to `ESP_VISION_WIFI_DEFAULT_SSID` and `ESP_VISION_WIFI_DEFAULT_PASSWORD`; saved NVS profiles and protocol fields retain their meaning.
- Unified CLI queries under `inspect`, with OS-aware USB serial discovery, macOS callout deduplication, Unicode-aware output and `--all-ports` for native UART/Bluetooth candidates. Explicit port connections remain available.
- Unified manual and CI hardware acceptance under `host/device-core` and `npm run test:hardware`. SDK checks/publication are separate from firmware builds; targeted board/IDF verification and the final build matrix remain available.

### Fixed

- Reserved 16 KiB for each EV-MUX transport-task stack to prevent stack-protection resets during nested Wi-Fi configuration parsing and hashing over USB or TCP.
- Retained the project description in firmware CI artifacts so release metadata binds to the actual build version. Release packages validate complete install/update metadata, image hashes, partition bounds and root publication paths before upload; historical releases without qualified metadata remain unavailable for Core flashing.
- Corrected English release-guide title underlines for strict HTML/PDF documentation builds.
- Fixed LCD framebuffer reuse before color DMA completion, balanced P4X-EYE display DMA/JPEG memory use and released resources after failed initialization. Camera and LCD capture use lazy, reusable buffers with lifecycle cleanup.
- Reserved P4X-EYE VFS registration headroom so repeated camera resets can recreate video devices after network and storage initialization.
- Bounded serial startup draining, same-link reacquisition and stale TCP socket reclamation; preserved ownership, partial-frame boundaries and epoch-scoped output across soft resets and backpressure.
- Unified Python and deferred USB DTR/RTS bootloader entry, preserving P4 revision 3.2 system-control bits and USB control completion before restarting.
- Prevented automatic formatting of damaged or unknown Flash FAT, reported storage writer/mount failures and guarded SD synchronization on unsupported boards.
- Aligned ESP-IDF hostnames with the 63-byte DNS limit, reserved DHCP option space and corrected mDNS 1.1 STA-only event/header guards.
- Fixed NanoTrack model compatibility and ESP32-P4 inference crashes; reduced S31 KORVO/CHATBOT camera capture buffers to two to leave more PSRAM for inference and display.
- Selected the supported ESP Video CSI format-conversion path on P4 releases with a backported API, including ESP-IDF 5.5 snapshots.
- Fixed Windows QSTR preprocessing with ESP-IDF 6.1 by evaluating response-file generator expressions and retaining required include paths; corrected CI image/tool availability and hardware-response parsing.
- Fixed standalone SDK exports and installed-package test assets, and avoided false npm publication failures while registry metadata propagates by retrying verification reads without repeating publication; 0.3.0 allows a bounded 151-second propagation backoff.
- Fixed README logos/architecture diagrams across light/dark GitHub and GitLab renderers, CLI column alignment, bilingual RST formatting and MCP/documentation references.

### Removed

- Removed duplicate Python EV-MUX clients, obsolete v3 stress/runtime tools and separate Agent smoke/recovery runners with their implementation-specific tests. Core hardware checks and native firmware contract/fault tests provide the maintained validation entry points.

## [2026.08.27]

### Added

- Added the shared ESP-DL PicoDet pedestrian model, an `espdl.PedestrianDetect` wrapper with model-specific preprocessing and post-processing, Python stubs, and a camera example for ESP32-P4/S31/S3 boards.
- Added a button-triggered PP-OCRv6 pipeline with separate INT8 text detection and recognition stages, Python detection/CTC post-processing, and generic BGR preprocessing through `espdl.Model`.
- Added ESP32-S3 USB-OTG CDC automatic download-mode entry using the esptool USB-Serial/JTAG DTR/RTS sequence, controlled per board from `mpconfigboard.h` and handled outside the MicroPython VM. The existing EV-MUX CDC stack now starts before camera initialization, filesystem recovery, and `boot.py`; its transport task keeps the download path responsive while the VM is busy without introducing a second USB driver or descriptor owner.
- Added an offline imlib allocator regression covering fallback allocation, nested marks, rollback, and failure cleanup.
- Added OpenMV v5.0.0's MIT-licensed Edge Drawing Lines implementation and enabled `Image.find_line_segments()` on all supported boards.
- Added initial `ESP32_S31_CHATBOT` support (requires ESP-IDF release/v6.1) with a USB Serial/JTAG console, an SC101IOT DVP camera, and a 480x480 CO5300 QSPI display managed by ESP Board Manager.
- Added initial `ESP32_S31_MOSAICO` support on ESP-IDF release/v6.1 with native USB 2.0 High Speed CDC/Flash MSC, an OV3640 DVP camera accepting both official product IDs (`0x364C` and `0x3641`), and a 480x480 CO5300 QSPI display. Cold-start eFuse detection treats the legacy all-zero value as v1.0 and selects the v1.0 or shared v1.1/v1.2 LCD pin map while retaining the CameraBoard's common wiring; camera startup enables the shared GPIO60 VCC_3V3 rail, releases the built-in USB Serial/JTAG pads shared by Camera D2, and waits for the external oscillator before SCCB probing. The generated `main.py` previews camera frames on the LCD with a default 90-degree counter-clockwise PPA rotation. The board uses onboard NAND Flash instead of an SD card; NAND storage is not yet exposed and camera flash support remains disabled.
- Added `ESP32_S31_MOSAICO` to the release manifest so tagged releases publish its merged firmware in the website's ESP32-S31 download and online-flashing selector.
- Added a camera-independent LCD color-bar and solid-color diagnostic under `example/06-Peripherals/01-Display`.

### Changed

- Updated the `ESP32_S31_MOSAICO` generated `main.py` to capture VGA, crop the center 480x480, and fit that square to the 480x480 display.
- Updated the ESP Video dependency to version 2.4.0 across the supported ESP-IDF release overlays.
- Added periodic MicroPython event polling to long-running imlib drawing, filtering, feature-detection, QR, statistics, and template-matching loops.
- Migrated supported boards' camera and LCD initialization to ESP Board Manager, including runtime OV3660 and SC101IOT format selection and sensor-specific orientation correction on `ESP32_S31_KORVO`; the `TEMPLATE` board remains an explicitly documented non-Board-Manager starting point.
- Synchronized the MicroPython Wi-Fi authentication constants with the native and remote Wi-Fi backends in ESP-IDF 6.0 and 6.1.
- Updated `esp_wifi_remote` to 1.6.4 for every supported ESP-IDF release overlay.
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
- Fixed the EV-MUX stress test to expect non-active-sink discovery responses on their request ingress, matching the protocol v3 RPC response contract.
- Fixed EV-MUX path validation so script/filesystem paths are limited to 123 bytes, leaving room for the transactional `.tmp` suffix within MicroPython's 128-byte VFS path capacity; over-limit paths return `PATH_TOO_LONG` without entering VFS or resetting the VM.
- Fixed Wi-Fi authentication constant exposure after the remote Wi-Fi header update by removing the deprecated WPA3 extension bindings that no longer exist in the active API; the IDF 6 unknown-scan mode remains exposed where that enum is provided.
- Fixed IDF 5.5 builds on AtomS3R-M12 by accepting both Wi-Fi authentication enum layouts used by the release and stable 5.5 headers without changing managed components.
- Fixed internal-preferred fast-framebuffer allocations incorrectly reporting exhaustion when the internal largest block was small but a usable PSRAM block remained; AprilTag, QR, display, and heap-backed UMM temporary allocations now unwind through balanced normal and exceptional paths.

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
