# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## Quickstart

Prerequisites: ESP-IDF v5.5.3 with the export script sourced (`idf.py` on `PATH`), and an ESP32-P4 target board.

```bash
git clone --recursive <this-repo> esp-vision
cd esp-vision
make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor
```

Other useful targets: `make menuconfig`, `make size`, `make erase`, `make clean`, `make distclean`.

## License

ESP-VISION's own code is released under the Apache License 2.0. Vendored code keeps the license declared in each file's SPDX header.

| Repository | Local path | Usage | License |
| --- | --- | --- | --- |
| [MicroPython](https://github.com/micropython/micropython) | `lib/micropython` | MicroPython runtime and ESP32 port base | MIT |
| [micropython-ulab](https://github.com/v923z/micropython-ulab) | `lib/ulab` | `ulab` numerical module | MIT |
| [OpenMV](https://github.com/openmv/openmv) `imlib` MIT subset (v4.8.1) | `components/imlib` | Image processing and drawing algorithms | MIT |
| [ESP-DL](https://github.com/espressif/esp-dl) | from ESP Component Registry | ESPDet model inference runtime | MIT |
| [ESP-IDF](https://github.com/espressif/esp-idf) | external SDK | ESP32-P4 build system, drivers, JPEG/PPA/camera related components | Apache-2.0 |
| [node-serialport](https://github.com/serialport/node-serialport) | `vscode-extension` npm dependency | VSCode extension serial transport | MIT |
| [TypeScript](https://github.com/microsoft/TypeScript) | `vscode-extension` dev dependency | VSCode extension build tool | Apache-2.0 |
| [VS Code API typings](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/vscode) | `vscode-extension` dev dependency | VSCode extension type definitions | MIT |
| [Node.js typings](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/node) | `vscode-extension` dev dependency | Node.js type definitions | MIT |
