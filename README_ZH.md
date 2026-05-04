# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## 快速开始

环境要求：ESP-IDF v5.5.3（已 source `export.sh`，`idf.py` 可用），以及 ESP32-P4 目标板。

```bash
git clone --recursive <本仓库> esp-vision
cd esp-vision
make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor
```

其他常用目标：`make menuconfig`、`make size`、`make erase`、`make clean`、`make distclean`。

## 许可证

ESP-VISION 自有代码以 Apache License 2.0 发布。引入的第三方代码保持各文件 SPDX 头中声明的原始许可证。

| 仓库 | 本地路径 | 用途 | 许可证 |
| --- | --- | --- | --- |
| [MicroPython](https://github.com/micropython/micropython) | `lib/micropython` | MicroPython 运行时及 ESP32 移植基础 | MIT |
| [micropython-ulab](https://github.com/v923z/micropython-ulab) | `lib/ulab` | `ulab` 数值计算模块 | MIT |
| [OpenMV](https://github.com/openmv/openmv) `imlib` MIT 子集 (v4.8.1) | `components/imlib` | 图像处理与绘制算法 | MIT |
| [ESP-DL](https://github.com/espressif/esp-dl) | 来自 ESP Component Registry | ESPDet 模型推理运行时 | MIT |
| [ESP-IDF](https://github.com/espressif/esp-idf) | 外部 SDK | ESP32-P4 构建系统、驱动、JPEG/PPA/Camera 等组件 | Apache-2.0 |
| [node-serialport](https://github.com/serialport/node-serialport) | `vscode-extension` npm 依赖 | VSCode 扩展串口传输 | MIT |
| [TypeScript](https://github.com/microsoft/TypeScript) | `vscode-extension` 开发依赖 | VSCode 扩展构建工具 | Apache-2.0 |
| [VS Code API typings](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/vscode) | `vscode-extension` 开发依赖 | VSCode 扩展类型定义 | MIT |
| [Node.js typings](https://github.com/DefinitelyTyped/DefinitelyTyped/tree/master/types/node) | `vscode-extension` 开发依赖 | Node.js 类型定义 | MIT |
