# Changelog

All notable changes to ESP-VISION are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/); each released version corresponds to a git tag. Unreleased changes accumulate at the top and are folded into the next tag at release time.

## [Unreleased]

### Added

- Device Core 0.3.0 adds explicit complete-firmware installation for blank devices through the existing firmware API/CLI and ROM adapters. Release metadata qualifies the merged bootloader, partition table, application, flash capacity and silicon revision; existing application-only updates remain the default.

- Add reliable EV-MUX binary file upload shared by Device Core files, script staging and action bindings: bounded 16 KiB chunks, continuous native VFS writes, SHA-256 commit and session/VM/timeout cleanup. Keep small control messages separate from payloads and allocate receive buffers only for authorized active frames.

- Device Core 0.1.1 exports passive Node TCP/serial adapters at `/node` and ships its unit/hardware check assets; installed-package validation executes the tests and provider-backed discovery from a clean consumer.

- Official release artifacts now include application-only update images and per-board/version metadata (SHA-256, partition layout and silicon revision bounds) for automatic Device Core/CLI updates from dl.espressif.com.

- Added a bilingual Device Core host SDK chapter with the complete application API, events/errors, all package exports and public advanced class methods, discovery and ROM flashing contracts.

- Added dedicated LCD JPEG capture (`debug.capture_screen`) of the last submitted RGB565 display frame through Device Core, with explicit uninitialized/unsupported errors and binary response metadata.

- Moved the independently versioned `@esp-vision/device-core` host SDK into `host/device-core`, with the grouped `createDevice()` API, standalone npm checks/dry runs and shared firmware/SDK documentation. IDE and Agent remain npm consumers.

- Added EV-MUX v4 ownership, leases, keepalive and bounded frame transmission across USB-Serial-JTAG, USB-OTG, UART and TCP, including session/VM-epoch isolation for REPL, preview and inherited Python threads.
- Added shared native/Hosted background Wi-Fi, transactional NVS provisioning and reconnect, an IPv4 TCP server, and explicit per-board default/fallback endpoint and service policies.
- Added session-bound storage-writer leases with MSC handoff/quarantine gates, typed launcher boot/trial status and MicroPython bindings, plus native contract/fault tests and hardware acceptance tools.
- Added lightweight `_esp-vision._tcp.local` discovery synchronized with the STA TCP listener and a shared mDNS lifecycle.
- Added one stable device ID and canonical hostname from the full board name, main-chip MAC suffix and firmware version, shared by USB/USJ/UART/TCP metadata, DHCP and Python hostname queries.
- Added a bilingual EV-MUX communication reference covering architecture, interfaces, ownership, wire framing, Python/files, preview/debug, validation and Device Core npm/IDE integration.

- Added `sensor.get_ipa_stats()` for IPA statistics, with documentation, an example, and CI hardware coverage.
- Added INT8 `espdl.NanoTrack` for automatic single-object tracking, with models and an example that continuously tracks a target after BOOT-button selection.
- Added the `lcd_touch.Touch` API with stubs, documentation, and touch examples with continuous drawing and reduced polling delay.
- Added runtime IPA and manual ISP control to the `sensor` module through `set_ipa()`, `get_ipa()`, `set_isp()`, and `get_isp()`, covering the exposure, pixel gain, demosaic, white balance, LSC, BLC, BF, sharpen, CCM, and gamma blocks over the ESP Video V4L2 controls. The feature is opt-in per board through `ESP_VISION_ENABLE_CAMERA_IPA_CONTROL` and is enabled on `ESP32_P4X_FUNCTION_EV_BOARD` and `ESP32_P4X_EYE`, with documentation in the `sensor` API reference and an example under `example/01-Camera/04-IPA`.
- Added a hardware-in-the-loop CI stage that flashes the `ESP32_P4X_FUNCTION_EV_BOARD` build onto a real board and tests its camera and LCD, through `tools/ci/flash_and_test.py` and the shared Device Core hardware-check entry point.

### Changed

- Updated SDK installation guides for the published Device Core 0.1.0 npm release; firmware integration remains independent of npm and IDE version numbers.

- Simplified the coding-agent guide into implementation and documentation indexes, including Device Core APIs/CLI, shared hardware acceptance, current build boundaries and release rules.

- Show IDE, Agent, CLI and custom host applications sharing Device Core above EV-MUX in the architecture diagram, labeling its interface with representative public API names, reusing the SVG across architecture/SDK guides and clarifying host, protocol, byte transport, discovery and ROM-flashing boundaries; keep npm badges at the top of both READMEs and move SDK usage details to its existing guide.

- Align the Device Core npm package license with firmware Apache-2.0, retaining historical MIT attributions and permissions in its distributed NOTICE file.

- Aligned Device Core CLI queries with `device.inspect()`, with OS-aware USB serial discovery, macOS callout deduplication and `--all-ports` for native UART/Bluetooth endpoints; explicit port connections remain available.
- Separate SDK-only CI and `device-core-vX.Y.Z` validation tags from firmware build/HIL jobs; retain explicit targeted-bin and final-matrix controls. The migrated SDK uses Apache-2.0 and retains historical MIT attributions in its distributed NOTICE.

- Renamed board default credential macros to `ESP_VISION_WIFI_DEFAULT_SSID` and `ESP_VISION_WIFI_DEFAULT_PASSWORD`; saved NVS profiles and protocol fields are unchanged.
- Unified hardware CI and manual acceptance under `host/device-core` and `npm run test:hardware`, removed the redundant Python EV-MUX clients/runners and their duplicate tests, aligned MCP guide validation, and retained native C contract checks; v3 clients must update to the corresponding v4 host implementation.
- Added optional single-board/IDF CI filters for experimental hardware validation; unfiltered final pipelines retain the complete build matrix and hardware tests.
- Separated EV-MUX protocol documentation from the Agent code-generation walkthrough, retained the former how-to entry as a redirect, and included communication pages in navigation and MCP document metadata; aligned system, SDK, host-integration and physical-link diagrams with Host → Device Core → EV-MUX → firmware and shared the canonical SVG.

- Upgraded ESP-DL to 3.3.13 and enabled dual-core NanoTrack inference on ESP32-P4.
- Optimized NanoTrack preprocessing and raised the example match threshold to 0.95.

### Fixed

- Retry bounded, uncached registry reads after npm publication while metadata propagates; keep publication single-shot and reject authentication failures or integrity mismatches immediately.

- Corrected bilingual communication guides to distinguish public Core file/Python APIs from low-level helpers, clarified firmware-unit versus Core hardware tests, and kept SDK documentation links within the current documentation version and target.

- Added theme-aware logos and architecture diagrams to both English and Chinese READMEs, with self-contained adaptive SVG fallbacks for GitLab renderers that strip picture sources, responsive 960-pixel logo sizing, and image backgrounds that keep text readable when site and browser themes differ.
- Fixed Device Core CLI discovery alignment with Unicode-aware column padding and device blocks on narrow terminals, preserving complete field values.
- Wait for LCD color DMA completion before reusing its framebuffer; share lazy, reusable camera snapshot storage between Python and Core capture, and release it on reconfiguration/shutdown/VM reset.

- Reserved VFS registration headroom on `ESP32_P4X_EYE` so repeated camera resets can recreate video devices after the network and storage interfaces have initialized.

- Bounded P4X-EYE SPI LCD bounce-buffer memory and balanced the dedicated DMA reserve against DEFAULT-capable JPEG allocations so display and capture work alongside Wi-Fi/TCP; release display resources when the initial clear fails.

- Report Flash mount/writer state alongside hardware-test upload failures without automatically formatting or clearing a storage quarantine.
- Unified Python bootloader/reset and deferred USB DTR/RTS entry through one ROM-download/restart path, preserving P4 revision 3.2 system control bits and USB control completion before reboot.
- Bounded startup draining, same-link reacquisition and stale TCP socket reclamation; preserved owner routing and partial-frame boundaries across soft resets and backpressure.
- Prevented damaged/unknown Flash FAT from being automatically formatted, guarded SDMMC synchronization on unsupported boards, and retained touch cleanup and required Launcher/storage headers across IDF builds.
- Aligned ESP-IDF network-interface hostnames to the 63-byte DNS limit, reserved DHCP option space, and matched the mDNS 1.1 Ethernet event/header guards for STA-only discovery.
- Corrected bilingual RST heading/literal formatting and MCP cross-reference validation for warning-free documentation builds.

- Fixed NanoTrack model compatibility and ESP32-P4 inference crashes.
- Fixed Windows QSTR preprocessing with ESP-IDF 6.1 by evaluating CMake generator expressions in compiler response files, including Mbed TLS header paths, and omitting empty include and definition entries.
- Fixed the hardware-in-the-loop device checks intermittently failing with `ValueError: not enough values to unpack` by parsing a device answer only once its whole line has arrived.
- Fixed the Python CI jobs failing with `wget: command not found` by moving them from `python:3.11-slim` to `python:3.11`.
- Reduced `ESP32_S31_KORVO` and `ESP32_S31_CHATBOT` to two camera capture buffers, freeing 1.76 MiB of PSRAM so a model still fits alongside the 1280x720 SC101IOT capture path and the LCD framebuffers.
- Enabled ESP Video CSI format conversion for supported ESP32-P4 revisions when the IDF CSI header exposes the backported API, fixing camera startup on release/v5.5 snapshots whose version macro still selects the legacy RAW-to-RGB CSI path.

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

- Removed the duplicate Python EV-MUX client, obsolete v3 stress/runtime tools, separate Agent smoke/recovery runners and their implementation-specific tests. Firmware HIL now uses the same Device Core hardware entry point while preserving camera, LCD and native C contract checks.

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
