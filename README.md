# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## Overview

ESP-VISION is Espressif's low-code framework for intelligent edge vision. It deeply integrates essential capabilities including camera capture, image processing, video encoding and decoding, network transmission, model deployment, and AI inference, while providing unified and standardized Python APIs that enable developers to rapidly build edge applications combining visual capture, intelligent recognition, display, and media streaming.

- Unified and standardized Python APIs for camera capture, image input, display, video encoding, preview, and streaming across supported chips and boards, enabling low-code application development.
- Image processing capabilities covering drawing, filtering, color tracking, feature detection, QR codes, barcodes, and AprilTags.
- ESP-DL-powered AI inference for object detection, pose estimation, and image classification, with a streamlined path for deploying models to edge devices.
- Efficient C/C++ foundation components work closely with on-chip multimedia peripherals and hardware acceleration modules to deliver high-performance, real-time application execution.
- Development through a VSCode-based host tool or [Web IDE](https://esp-vision-ide.espressif.tools/), with firmware builds managed through `idf.py`.

**Document Center**:

- [中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [English](https://docs.espressif.com/projects/esp-vision/en/latest/)

## Architecture

![ESP-VISION layered architecture](docs/_static/esp-vision-architecture.svg)

See [Solution architecture and licensing](https://docs.espressif.com/projects/esp-vision/en/latest/architecture/index.html) for the role of each layer.

## Quickstart

Prerequisites: ESP-IDF release/v5.5, release/v6.0, or master with the export script sourced (`idf.py` on `PATH`), and a [supported board](https://docs.espressif.com/projects/esp-vision/en/latest/target-support/index.html).

```bash
git clone --recursive https://github.com/espressif/esp-vision.git esp-vision
cd esp-vision
idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor
```

- `--board ESP32_P4X_EYE`: Selects the board configuration; replace `ESP32_P4X_EYE` with another supported board when required.
- `-p /dev/ttyACM0`: Selects the device port; replace `/dev/ttyACM0` with the port reported by the development host.
- `build`: Configures the project and compiles the firmware.
- `flash`: Writes the generated firmware image to the connected device.
- `monitor`: Opens the serial console after flashing; press `Ctrl-]` to exit.

## ESP-IDF Compatibility

| ESP-IDF branch | Support | Standard MicroPython features | Notes |
| --- | --- | --- | --- |
| `release/v5.5` | Supported | Provides the more complete feature set, including SSL/TLS, `cryptolib`, WebSocket, WebREPL, socket events, and, on supported chips, `machine.I2S`, ESP32 PCNT, and I2C target mode. | Recommended when these optional MicroPython features are required. |
| `release/v6.0` | Supported | Provides `network`, WLAN, and sockets; the optional features listed for `release/v5.5` remain disabled with the current ESP-VISION overlay. | Does not currently support ESP32-S31 builds. |
| `master` | Supported | Provides `network`, WLAN, and sockets; the optional features listed for `release/v5.5` remain disabled with the current ESP-VISION overlay. | Required for ESP32-S31. |

## Demos

|||
| --- | --- |
| Hand Detect | Cat Detect |
| ![Hand Detect demo](https://dl.espressif.com/AE/esp-vision/hand-detection.gif) | ![Cat Detect](https://dl.espressif.com/AE/esp-vision/cat-detection.gif) |
| AprilTag | Canny |
| ![AprilTag demo](https://dl.espressif.com/AE/esp-vision/apriltag.gif) | ![Canny demo](https://dl.espressif.com/AE/esp-vision/edge-detection.gif) |
| Color Detect | QR Code |
| ![Color detection demo](https://dl.espressif.com/AE/esp-vision/color-recognition.gif) | ![QR code demo](https://dl.espressif.com/AE/esp-vision/qr-code-recognition.gif) |
|||

## Resources

- [Supported Chips and Boards](https://docs.espressif.com/projects/esp-vision/en/latest/target-support/index.html)
- [Web IDE](https://esp-vision-ide.espressif.tools/)
- [Examples](example/)
- [Customize firmware features](https://docs.espressif.com/projects/esp-vision/en/latest/how-to/customize-firmware.html)
- [Solution architecture and licensing](https://docs.espressif.com/projects/esp-vision/en/latest/architecture/index.html)
