# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESP-VISION is a MicroPython vision runtime for ESP32-P4 / ESP32-S3 boards. It builds a custom firmware: the upstream MicroPython esp32 port plus an overlay, ESP-VISION C modules (`sensor`, `image`, `display`, `espdl`, plus the `image.ImageIO` stream type), a self-written platform layer (camera, preview, display, storage, USB), and OpenMV's MIT `imlib` as a vision component. The VSCode host tool lives in a separate repo.

## Build Commands

The top-level `Makefile` is the only supported entry point — it wraps `idf.py` with the right flags. The real IDF project root is `lib/micropython/ports/esp32`; never create a standalone IDF app at the repo root.

- Build: `make BOARD=ESP32_P4X_EYE build`
- Build + flash: `make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 deploy`
- Serial monitor: `make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 monitor`
- Menuconfig: `make BOARD=ESP32_P4X_EYE menuconfig`
- Size: `make BOARD=ESP32_P4X_EYE size`
- Erase flash: `make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 erase`
- Clean board build output: `make BOARD=ESP32_P4X_EYE clean`
- Wipe all build output: `make distclean`
- Requires ESP-IDF release/v5.5 with `idf.py` on `PATH` (source the IDF `export.sh` if it isn't).

Notes:
- `BOARD` must exist in both `boards/<BOARD>/` and `overlay/micropython/ports/esp32/boards/<BOARD>/`. Boards: `ESP32_P4X_EYE`, `ESP32_P4X_FUNCTION_EV_BOARD`, `ESP32_S3_EYE`; `TEMPLATE` is for new-board bring-up.
- After changing the build system, board config, platform drivers, or imlib options, verify the `ESP32_P4X_EYE` build first; other boards only when the task touches them.
- Firmware build/flash/monitor/config targets first run `prepare-micropython`: it asserts `lib/micropython` is at the pinned commit (`v1.28.0`, `e0e9fbb17ed6fd06bb76e266ae554784c9c80804`), then copies `overlay/micropython/` over the submodule working tree. Modified/untracked files in the submodule after a build are expected — not a dirty checkout to clean.

## Lint & Checks

There is no test suite; CI runs `pre-commit` and a firmware build matrix.

- C/C++ formatting (astyle 3.4.7): `pre-commit run --all-files astyle_py`
- Copyright/SPDX headers: `pre-commit run --all-files check-copyright`
- Stub syntax (mirrors `python_stubs` CI job): `python -m py_compile stubs/*.pyi`
- `components/imlib/`, `lib/micropython/`, `lib/ulab/`, `overlay/micropython/`, and OpenMV-sourced `modules/py_{helper,image,imageio,assert}.*` are excluded from formatting/copyright rewriting — do not re-include them or let the hooks touch upstream headers.

## Project Structure

Layered by whether code touches MicroPython (`mp_obj_t` / `py/*.h`):

- `components/imlib/`: pure-C vision algorithms, IDF component, maintained as MIT. `upstream/` = OpenMV `lib/imlib` MIT files (keep close to upstream, record any adaptation); `include/` = contract headers + CMSIS shims (no `.c`); `compat/` = ESP-VISION contract impls (`array.c`, `fb_alloc.c`, `umm_malloc.c`).
- `platform/`: self-written ESP32 glue, may couple with MicroPython. `preview.c` (EVFRAME JPEG preview over USB CDC; `img.flush()` calls `esp_vision_preview_flush`), `display.c` (generic layer; board provides panel/backlight via `esp_vision_board_display_*` hooks), `sdcard.c` (generic mount at `/sdcard`; board provides `esp_vision_board_sdcard_*` weak hooks), `usb_msc.c` (exposes the on-flash FAT data partition `ffat`/`vfs` over TinyUSB MSC), `jpeg.c` (hardware JPEG or `esp_new_jpeg` software fallback), `debug.c`. `main.c` replaces upstream MP `main.c` and owns startup init + the soft-reset loop. `camera.c` here is only weak-symbol placeholders (`ESP_ERR_NOT_SUPPORTED`) plus the shared framesize table; the real backend lives per-board in `boards/<BOARD>/camera.c` (P4X uses `esp_video`/V4L2 + PPA, S3 uses `esp32-camera` — no esp_video/PPA).
- `modules/`: the `USER_C_MODULES` binding layer (`py_*`). The four real MicroPython modules — `image`, `sensor`, `display`, `espdl` — self-register via `MP_REGISTER_MODULE`. `py_imageio.c` provides the `image.ImageIO` type and `py_helper.c` is shared binding-layer helper code (neither is a standalone module). Board-feature-gated qstrs live in `modules/qstrdefs_esp_vision.h`.
- `boards/<BOARD>/`: per-board config — `boardconfig.h` (`ESP_VISION_*` macros), `imlib_config.h` (imlib switches), `manifest.py` (frozen modules), optional `camera.c`/`display.c`/`sdcard.c` overriding platform defaults, optional `board.cmake` setting `ESP_VISION_ENABLE_BARCODE` to opt into the zxing-cpp 1D backend.
- `micropython.cmake`: the wiring hub — injects `platform/main.c` as MP main source, lists module/platform sources + include paths, picks per-board backends when present, conditionally links `idf::zxing`, pulls in `ulab`.
- `example/`: numbered MicroPython example scripts; `stubs/`: `.pyi` stubs for the C modules; `models/`: optional `.espdl` assets loaded at runtime.

A board's definition is split across two trees: the MicroPython port side (`overlay/micropython/ports/esp32/boards/<BOARD>/`: IDF target, sdkconfig, partitions, USB strings) and the ESP-VISION side (`boards/<BOARD>/`).

## Code Style & Conventions

- Public Python module names stay OpenMV-compatible: the camera module is `sensor` (not `camera`); `image`, `display`, `espdl` alongside, with the codec stream exposed as `image.ImageIO`.
- `omv_*` prefixes only appear under `components/imlib/upstream|include/`. `platform/` and `modules/` use ESP-VISION names. Board names stay `ESP32_*` UPPER_SNAKE, aligned with `MICROPY_BOARD`.
- Bindings (`modules/`) only do object conversion + light API adaptation; push heavy logic into pure C or `platform/`.
- Code touching DMA, PSRAM, cache coherency, PPA, or USB CDC/MSC must handle error paths explicitly. Avoid dynamic allocation on hot paths — reuse the framebuffer / `fb_alloc` / lifetime-scoped buffers.
- Prefer changes via `overlay/` and `USER_C_MODULES` over editing `lib/micropython/`; only patch the submodule when genuinely unavoidable.
- Don't commit build output, temporary sdkconfig files, logs, or local device paths.

## Licensing (high priority — this repo will be open-sourced)

How license is assigned across the tree:

- Repo-original code (`platform/`, most of `modules/`, `boards/`, build files) is `SPDX-License-Identifier: Apache-2.0`. The `check-copyright` pre-commit hook enforces the Espressif Apache header on these files.
- `components/imlib/` is maintained as MIT (OpenMV-derived). The one exception is `components/imlib/upstream/apriltag.c` (BSD-2-Clause); the README license table is the source of truth for per-component licenses.
- `modules/py_{image,helper,imageio,assert}.*` are OpenMV MIT files kept with their original headers verbatim, and are excluded from the formatting/copyright hooks so the upstream headers are never rewritten.
- Vendored submodules keep their own license: `lib/micropython` (MIT), `lib/ulab` (MIT), `lib/zxing-cpp` (Apache-2.0). Registry components (ESP-DL, esp_new_jpeg, esp32-camera) keep theirs.
- `OMV_NO_GPL=1` (set in `micropython.cmake` and `components/imlib/CMakeLists.txt`) keeps GPL-conditional OpenMV code (e.g. `agast.c`) out of the build — do not enable those paths.

When adding third-party source: verify the license per file (don't infer it from the directory or repo), preserve the original header, and add an entry to the README license table. When a license is unclear, stop and ask rather than guess.
