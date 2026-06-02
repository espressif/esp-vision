# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## Quickstart

Prerequisites: ESP-IDF release/v5.5 with the export script sourced (`idf.py` on `PATH`), and a supported ESP32-P4 or ESP32-S3 target board.

```bash
git clone --recursive <this-repo> esp-vision
cd esp-vision
make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor
```

Set `BOARD` to the board package name under `boards/` and `overlay/micropython/ports/esp32/boards/`.

Other useful targets: `make menuconfig`, `make size`, `make erase`, `make clean`, `make distclean`.

Before invoking `idf.py`, the top-level Makefile runs `prepare-micropython`: it verifies that `lib/micropython` is checked out at MicroPython v1.28.0 commit `e0e9fbb17ed6fd06bb76e266ae554784c9c80804`, then applies `overlay/micropython/` to the submodule working tree.

## Architecture

ESP-VISION is an ESP32-P4/ESP32-S3 MicroPython vision runtime with a VSCode-based host tool.

### Source Tree

| Path | Responsibility |
| --- | --- |
| `Makefile` | Top-level build entry that wraps `idf.py` with the correct MicroPython ESP32 port flags. |
| `micropython.cmake` | ESP-VISION MicroPython integration point, registering user modules, platform sources, board sources, include paths, and `ulab`. |
| `lib/micropython` | MicroPython v1.28.0 submodule used as the upstream baseline. |
| `lib/ulab` | Pinned `micropython-ulab` submodule used for numerical Python support. |
| `overlay/micropython` | ESP-VISION MicroPython delta, using the same path layout as the MicroPython repository. |
| `boards` | Board packages containing per-board configuration, frozen manifests, and board-specific peripheral implementations. |
| `platform` | Runtime services shared by Python modules, including camera, preview, storage, display, USB, JPEG, and debug support. |
| `modules` | MicroPython C/C++ bindings exposed to scripts, including `sensor`, `image`, `display`, `imageio`, and `espdl`. |
| `components/imlib` | ESP-IDF component containing the selected OpenMV `imlib` sources and the ESP32 compatibility layer. |
| `models` | Optional `.espdl` model assets loaded at runtime from board storage such as `/flash` or `/sdcard`. |
| `example` | MicroPython example scripts for camera, preview, storage, display, image processing, and ESP-DL workflows. |

### MicroPython Overlay

ESP-VISION uses MicroPython v1.28.0 as a fixed upstream baseline. Project-specific changes to the MicroPython ESP32 port are maintained under `overlay/micropython/`.

`prepare-micropython` applies the overlay to `lib/micropython/`. Modified or untracked files corresponding to the overlay are expected in the submodule after preparation.

### Adding a New Board

A board package is split across two locations: the MicroPython ESP32 port overlay (`overlay/micropython/ports/esp32/boards/<BOARD>/`) and the ESP-VISION repo (`boards/<BOARD>/`).

**1. MicroPython port side** — `overlay/micropython/ports/esp32/boards/<BOARD>/`:

| File | Purpose |
| --- | --- |
| `mpconfigboard.cmake` | IDF target and `SDKCONFIG_DEFAULTS` chain. |
| `mpconfigboard.h` | MicroPython feature flags and USB strings. |
| `sdkconfig.board` | Board-specific ESP-IDF Kconfig overrides. |
| `partitions-*.csv` | Partition table. |
| `board.json`, `board.md` | Upstream board manifest metadata. |

**2. ESP-VISION side** — `boards/<BOARD>/`:

| File | Purpose |
| --- | --- |
| `boardconfig.h` | Pin assignments and board runtime constants. |
| `imlib_config.h` | OpenMV `imlib` feature switches. |
| `manifest.py` | Frozen Python modules. |
| `camera.c` | Board-specific camera backend. |
| `display.c` | LCD panel and backlight implementation. |
| `sdcard.c` | SD card power and card-detect implementation. |

**3. Build and flash**:

```bash
make BOARD=<NEW_BOARD> ESPPORT=/dev/ttyACM0 build flash monitor
```

## License

ESP-VISION's own code is released under the Apache License 2.0. Vendored code keeps the license declared in each file's SPDX or license header.

| Repository | Local path | Usage | License |
| --- | --- | --- | --- |
| [MicroPython](https://github.com/micropython/micropython) | `lib/micropython` | MicroPython runtime and ESP32 port base | MIT |
| [micropython-ulab](https://github.com/v923z/micropython-ulab) | `lib/ulab` | `ulab` numerical module | MIT |
| [OpenMV](https://github.com/openmv/openmv) `imlib` MIT subset, excluding files listed separately (v4.8.1) | `components/imlib` | Image processing and drawing algorithms | MIT |
| AprilTag algorithm from OpenMV `imlib` | `components/imlib/upstream/apriltag.c` | AprilTag and rectangle detection | BSD-2-Clause |
| [ZXing-C++](https://github.com/zxing-cpp/zxing-cpp) | `lib/zxing-cpp` | 1D barcode reader backend | Apache-2.0 |
| [ESP-DL](https://github.com/espressif/esp-dl) | from ESP Component Registry | Model inference runtime | MIT |
| [esp_new_jpeg](https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg) | from ESP Component Registry | Software JPEG codec library | Espressif MIT |
| [esp32-camera](https://github.com/espressif/esp32-camera) | from ESP Component Registry | Camera driver library | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | external SDK | ESP32 build system, drivers, JPEG/PPA/camera related components | Apache-2.0 |
