<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/_static/ESP-VISION_logo_dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/_static/ESP-VISION_logo.svg">
    <img src="docs/_static/ESP-VISION_logo_adaptive.svg" alt="ESP-VISION logo" width="960">
  </picture>
</p>

# ESP-VISION

[![Device Core npm version](https://img.shields.io/npm/v/%40esp-vision%2Fdevice-core?logo=npm&label=Device%20Core)](https://www.npmjs.com/package/@esp-vision/device-core) [![Device CLI npm version](https://img.shields.io/npm/v/%40esp-vision%2Fdevice-cli?logo=npm&label=Device%20CLI)](https://www.npmjs.com/package/@esp-vision/device-cli) [![Knowledge MCP npm version](https://img.shields.io/npm/v/%40esp-vision%2Fknowledge-mcp?logo=npm&label=Knowledge%20MCP)](https://www.npmjs.com/package/@esp-vision/knowledge-mcp) [![Plugin npm version](https://img.shields.io/npm/v/%40esp-vision%2Fplugin?logo=npm&label=Plugin)](https://www.npmjs.com/package/@esp-vision/plugin) [![License](https://img.shields.io/github/license/espressif/esp-vision?label=License)](LICENSE)

**Low-Code Edge AI & Computer Vision Framework for Espressif SoCs**

[English](README.md) | [简体中文](README_ZH.md)

## Overview

ESP-VISION is a Low-Code Edge AI & Computer Vision Framework for Espressif SoCs. It deeply integrates essential capabilities including camera capture, image processing, video encoding and decoding, network transmission, model deployment, and AI inference, while providing unified and standardized Python APIs that enable developers to rapidly build edge applications combining visual capture, intelligent recognition, display, and media streaming.

- Unified and standardized Python APIs for camera capture, image input, display, video encoding, preview, and streaming across supported chips and boards, enabling low-code application development.
- Image processing capabilities covering drawing, filtering, color tracking, feature detection, QR codes, barcodes, and AprilTags.
- ESP-DL-powered AI inference for object detection, pose estimation, and image classification, with a streamlined path for deploying models to edge devices.
- Efficient C/C++ foundation components work closely with on-chip multimedia peripherals and hardware acceleration modules to deliver high-performance, real-time application execution.
- Develop with a VSCode-based host tool, [Web IDE](https://ide.vision.espressif.com/), standalone [Device CLI](host/device-cli/README.md), or supported AI clients through the official [plugin](host/esp-vision-plugin/README.md); manage firmware builds through `idf.py`.

**Document Center**:

- [中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [English](https://docs.espressif.com/projects/esp-vision/en/latest/)

## EV-MUX: Host Communication

EV-MUX v4 provides IDEs, host tools and independent Agent adapters with discovery, control, framed Python interaction and JPEG preview. Board-enabled `usj`, `usb`, `uart` and `tcp` links share one owner session, which owns both user/debug streams.

The [Communication System and EV-MUX chapter](docs/en/communication/index.rst) is the detailed reference for [console/log architecture](docs/en/communication/architecture.rst), [interfaces and board configuration](docs/en/communication/interfaces.rst), [ownership and recovery](docs/en/communication/sessions.rst), [wire protocol](docs/en/communication/wire-protocol.rst), files, preview, Wi-Fi provisioning and acceptance.

v4 still uses `EVMUX/1` framing. Hosts must implement explicit acquisition and keepalive; `route.bind`/`route.auto` are unsupported. Runtime `capabilities` determines available interfaces. TCP has no authentication or encryption in this implementation.

## Architecture

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/_static/esp-vision-architecture.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/_static/esp-vision-architecture-light.svg">
  <img src="docs/_static/esp-vision-architecture-adaptive.svg" alt="ESP-VISION layered architecture" width="960" height="1048">
</picture>

IDE and custom SDK applications use Device Core directly. AI clients and independent agents use the plugin Skill, invoke its bundled Device CLI for local device operations, and query the official online Knowledge MCP over HTTPS. Device CLI uses Device Core; EV-MUX connects the SDK to device firmware. See [Solution architecture and licensing](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/architecture/index.html) for the role of each layer.

For AI clients, the [local plugin](host/esp-vision-plugin/README.md) bundles a fixed [Device CLI](host/device-cli/README.md) and connects to the official online Knowledge MCP. The main repository owns knowledge/server artifacts; website deploys verified immutable images. See [MCP architecture and release flow](docs/mcp-release.md). Install a published version with `npx -y @esp-vision/plugin install codex` or `npx -y @esp-vision/plugin install claude`. For WorkBuddy, `npx -y @esp-vision/plugin install workbuddy` prepares a local Skill ZIP and official Knowledge MCP configuration; complete the documented imports in the WorkBuddy UI.

The [license inventory](docs/en/license/index.rst) records upstream components and adaptations, including the MIT-licensed MicroPython execution and thread overlays used for EV-MUX session isolation.

## Host packages

Each npm package has its own version, independent of firmware release tags. The badges above follow the public npm `latest` versions.

| npm package | Responsibility | Documentation |
| --- | --- | --- |
| [@esp-vision/device-core](https://www.npmjs.com/package/@esp-vision/device-core) | Host SDK for sessions, Python, files, capture and firmware operations | [Device Core](host/device-core/README.md) |
| [@esp-vision/device-cli](https://www.npmjs.com/package/@esp-vision/device-cli) | Standalone `esp-vision-device` command built on Device Core | [Device CLI](host/device-cli/README.md) |
| [@esp-vision/knowledge-mcp](https://www.npmjs.com/package/@esp-vision/knowledge-mcp) | Knowledge server and versioned snapshot, with stdio and HTTP transports | [Knowledge MCP](host/knowledge-mcp/README.md) |
| [@esp-vision/plugin](https://www.npmjs.com/package/@esp-vision/plugin) | Skill, bundled Device CLI and official online Knowledge MCP configuration for Codex, Claude Code and WorkBuddy | [Plugin installation](host/esp-vision-plugin/README.md) |

The official plugin uses `https://mcp.vision.espressif.com/mcp`; knowledge is maintained centrally. The Knowledge npm package also supports self-hosting.

## Quickstart

Prerequisites: ESP-IDF v5.5.6 for ESP32-P4/S3, or release/v6.1 for ESP32-S31 with the export script sourced (`idf.py` on `PATH`), and a [supported board](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/target-support/index.html).

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

> Note: When building on Windows, install [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) so the build can create MicroPython's host-side `mpy-cross.exe`. In the Visual Studio Build Tools installer, select the "Desktop development with C++" workload and make sure the "MSVC v143 - VS 2022 C++ x64/x86 build tools" component is selected.

## ESP-IDF Compatibility

CI builds and published firmware use the following board/IDF combinations:

| Boards | ESP-IDF dependency | CI image |
| --- | --- | --- |
| ESP32-P4 and ESP32-S3 boards (including `TEMPLATE`) | **v5.5.6** | `espressif/idf:v5.5.6` |
| ESP32-S31 boards (`ESP32_S31_CHATBOT`, `ESP32_S31_KORVO`, `ESP32_S31_MOSAICO`) | **release/v6.1** | `espressif/idf:release-v6.1` |

Use the matching ESP-IDF checkout and source its `export.sh` before invoking `idf.py`. v5.5.6 is pinned to a patch release; `release/v6.1` follows the maintained 6.1 branch required by S31. Other ESP-IDF versions, including newer patches, release/v6.0 and master, are outside this CI qualification matrix. ESP-IDF or managed-component API changes can cause configuration/build failures, change runtime behavior or alter the Python APIs exposed by MicroPython. Such builds may need source, overlay or dependency adjustments and require their own hardware verification.

With the current overlay, v5.5.6 provides SSL/TLS, `cryptolib`, WebSocket, WebREPL, socket events and, where the chip supports them, `machine.I2S`, PCNT and I2C target mode. These optional features remain disabled on the 6.1 build; inspect the actual firmware before relying on them.

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

## Report a problem

Report all ESP-VISION problems in [espressif/esp-vision GitHub Issues](https://github.com/espressif/esp-vision/issues), including problems in the IDE, website, Host Device Core, and firmware. Search existing issues first, then choose one of the four forms below. All four forms use English.

| Category | Scope | Report form |
| --- | --- | --- |
| IDE | Editor, projects, UI, preview, and flashing workflows | [Report an IDE problem](https://github.com/espressif/esp-vision/issues/new?template=ide.yml) |
| Website | Website pages, documentation, downloads, links, and MCP knowledge | [Report a website problem](https://github.com/espressif/esp-vision/issues/new?template=website.yml) |
| Host Device Core | Host connections, EV-MUX sessions, RPC, transport, and reconnect | [Report a Host Device Core problem](https://github.com/espressif/esp-vision/issues/new?template=host-device-core.yml) |
| Firmware | Builds, boot, crashes, camera, display, storage, and on-device APIs | [Report a firmware problem](https://github.com/espressif/esp-vision/issues/new?template=firmware.yml) |

Each form asks for environment and version details, reproduction steps, expected and actual behavior, and relevant logs or attachments. If the category is unclear, choose the closest match and describe the affected workflow. The form sources are in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/).

## Resources

- [Device Core SDK and API reference](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/index.html)

- [Supported Chips and Boards](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/target-support/index.html)
- [Web IDE](https://ide.vision.espressif.com/)
- [Examples](example/)
- [Customize firmware features](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/how-to/customize-firmware.html)
- [Solution architecture and licensing](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/architecture/index.html)
