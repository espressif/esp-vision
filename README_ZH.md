# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## 概述

ESP-VISION 是乐鑫面向端侧智能视觉打造的低代码开发框架，深度整合了摄像头采集、图像处理、视频编解码、网络传输、模型部署和 AI 推理等核心能力，并提供统一标准化的 Python 接口，赋能开发者快速构建集视觉采集、智能识别、画面显示与流媒体传输于一体的边缘应用。

- 为受支持的芯片和开发板统一提供标准化的 Python 接口，覆盖摄像头采集、图像输入、显示、视频编码、预览与推流，支持低代码应用开发。
- 提供绘图、滤波、颜色追踪、特征检测、二维码、条码和 AprilTag 等图像处理能力。
- 基于 ESP-DL 提供目标检测、姿态估计和图像分类等 AI 推理能力，并简化端侧模型部署流程。
- 底层高效的 C/C++ 基础组件深度协同芯片的多媒体外设与硬件加速模块，切实保障应用的高效与实时运行性能。
- 可通过 VSCode 上位机工具或 [Web IDE](https://esp-vision-ide.espressif.tools/) 开发，并使用 `idf.py` 管理固件构建。

**文档中心**：

- [中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [English](https://docs.espressif.com/projects/esp-vision/en/latest/)

## 方案架构

![ESP-VISION 分层架构](docs/_static/esp-vision-architecture.svg)

各层的职责说明见 [方案架构与许可证](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/architecture/index.html)。

## 快速开始

环境要求：ESP-IDF release/v5.5、release/v6.0 或 master（已 source `export.sh`， `idf.py` 可用），以及一块 [受支持的开发板](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/target-support/index.html)。

```bash
git clone --recursive https://github.com/espressif/esp-vision.git esp-vision
cd esp-vision
idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor
```

- `--board ESP32_P4X_EYE`：选择开发板配置；需要使用其他开发板时，请将 `ESP32_P4X_EYE` 替换为受支持的开发板。
- `-p /dev/ttyACM0`：选择设备端口；请将 `/dev/ttyACM0` 替换为开发主机识别到的端口。
- `build`：配置项目并编译固件。
- `flash`：将生成的固件镜像烧录到所连接的设备。
- `monitor`：烧录后打开串口终端；按 `Ctrl-]` 退出。

## ESP-IDF 兼容性

| ESP-IDF 分支 | 支持状态 | 标准 MicroPython 功能 | 说明 |
| --- | --- | --- | --- |
| `release/v5.5` | 支持 | 提供更完整的功能集，包括 SSL/TLS、`cryptolib`、WebSocket、WebREPL、socket events，以及受支持芯片上的 `machine.I2S`、ESP32 PCNT 和 I2C target 模式。 | 需要上述可选 MicroPython 功能时推荐使用。 |
| `release/v6.0` | 支持 | 提供 `network`、WLAN 和 socket；使用当前 ESP-VISION overlay 时，`release/v5.5` 中列出的可选功能仍保持关闭。 | 当前不支持构建 ESP32-S31。 |
| `master` | 支持 | 提供 `network`、WLAN 和 socket；使用当前 ESP-VISION overlay 时，`release/v5.5` 中列出的可选功能仍保持关闭。 | ESP32-S31 必须使用此分支。 |

## 效果演示

|||
| --- | --- |
| Hand Detect | Cat Detect |
| ![Hand Detect demo](https://dl.espressif.com/AE/esp-vision/hand-detection.gif) | ![Cat Detect](https://dl.espressif.com/AE/esp-vision/cat-detection.gif) |
| AprilTag | Canny |
| ![AprilTag demo](https://dl.espressif.com/AE/esp-vision/apriltag.gif) | ![Canny demo](https://dl.espressif.com/AE/esp-vision/edge-detection.gif) |
| Color Detect | QR Code |
| ![Color detection demo](https://dl.espressif.com/AE/esp-vision/color-recognition.gif) | ![QR code demo](https://dl.espressif.com/AE/esp-vision/qr-code-recognition.gif) |
|||

## 资源

- [芯片与开发板支持](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/target-support/index.html)
- [Web IDE](https://esp-vision-ide.espressif.tools/)
- [示例](example/)
- [客制化固件功能](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/how-to/customize-firmware.html)
- [方案架构与许可证](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/architecture/index.html)
