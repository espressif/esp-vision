<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/_static/ESP-VISION_logo_dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/_static/ESP-VISION_logo.svg">
    <img src="docs/_static/ESP-VISION_logo_adaptive.svg" alt="ESP-VISION logo" width="960">
  </picture>
</p>

# ESP-VISION

[![npm version](https://img.shields.io/npm/v/%40esp-vision%2Fdevice-core?logo=npm)](https://www.npmjs.com/package/@esp-vision/device-core) [![npm license](https://img.shields.io/npm/l/%40esp-vision%2Fdevice-core)](host/device-core/LICENSE)

**Low-Code Edge AI & Computer Vision Framework for Espressif SoCs**

[English](README.md) | [简体中文](README_ZH.md)

## 概述

ESP-VISION 是面向乐鑫 SoC 的低代码端侧 AI 与计算机视觉框架，深度整合了摄像头采集、图像处理、视频编解码、网络传输、模型部署和 AI 推理等核心能力，并提供统一标准化的 Python 接口，赋能开发者快速构建集视觉采集、智能识别、画面显示与流媒体传输于一体的边缘应用。

- 为受支持的芯片和开发板统一提供标准化的 Python 接口，覆盖摄像头采集、图像输入、显示、视频编码、预览与推流，支持低代码应用开发。
- 提供绘图、滤波、颜色追踪、特征检测、二维码、条码和 AprilTag 等图像处理能力。
- 基于 ESP-DL 提供目标检测、姿态估计和图像分类等 AI 推理能力，并简化端侧模型部署流程。
- 底层高效的 C/C++ 基础组件深度协同芯片的多媒体外设与硬件加速模块，切实保障应用的高效与实时运行性能。
- 可通过 VSCode 上位机工具或 [Web IDE](https://ide.vision.espressif.com/) 开发，并使用 `idf.py` 管理固件构建。

**文档中心**：

- [中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [English](https://docs.espressif.com/projects/esp-vision/en/latest/)

## EV-MUX：主机通信

EV-MUX v4 为 IDE、主机工具及独立 Agent 适配器提供发现、控制、带帧格式的 Python 交互和 JPEG 预览。开发板启用的 `usj`、`usb`、`uart`、`tcp` 链路共用唯一 owner 会话，由该会话同时拥有 user/debug 两条 stream。

[通信系统与 EV-MUX](docs/zh_CN/communication/index.rst) 是详细说明入口，涵盖 [console/日志架构](docs/zh_CN/communication/architecture.rst)、[接口与板级配置](docs/zh_CN/communication/interfaces.rst)、[所有权与恢复](docs/zh_CN/communication/sessions.rst)、[线上协议](docs/zh_CN/communication/wire-protocol.rst)、文件、预览、Wi-Fi 配置及验收。

v4 继续使用 `EVMUX/1` 帧格式。主机必须显式接管并保活，不支持 `route.bind`/`route.auto`。可用接口以运行时 `capabilities` 为准；当前 TCP 实现不提供认证或加密。

## 方案架构

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/_static/esp-vision-architecture.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/_static/esp-vision-architecture-light.svg">
  <img src="docs/_static/esp-vision-architecture-adaptive.svg" alt="ESP-VISION 分层架构" width="960" height="1066">
</picture>

IDE、Agent、CLI 和自定义工具共用 Device Core，通过 EV-MUX 与设备固件通信。各层职责见 [方案架构与许可证](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/architecture/index.html)。

[许可证清单](docs/zh_CN/license/index.rst)记录了上游组件与适配文件，包括用于 EV-MUX 会话隔离、保留 MIT 许可证的 MicroPython 执行与线程 overlay。

## 快速开始

环境要求：ESP32-P4/S3 使用 ESP-IDF v5.5.6，ESP32-S31 使用 release/v6.1（已 source `export.sh`，`idf.py` 可用），以及一块 [受支持的开发板](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/target-support/index.html)。

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

> 注意：在 Windows 平台编译时，需要安装 [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)，用于生成 MicroPython 主机侧工具 `mpy-cross.exe`。在 Visual Studio Build Tools 安装器中，请勾选 “Desktop development with C++”（使用 C++ 的桌面开发）工作负载，并确认已勾选 “MSVC v143 - VS 2022 C++ x64/x86 build tools” 组件。

## ESP-IDF 兼容性

CI 构建和发布固件使用以下开发板与 IDF 组合：

| 开发板 | ESP-IDF 依赖 | CI 镜像 |
| --- | --- | --- |
| ESP32-P4 和 ESP32-S3 开发板（包括 `TEMPLATE`） | **v5.5.6** | `espressif/idf:v5.5.6` |
| ESP32-S31 开发板（`ESP32_S31_CHATBOT`、`ESP32_S31_KORVO`、`ESP32_S31_MOSAICO`） | **release/v6.1** | `espressif/idf:release-v6.1` |

运行 `idf.py` 前，请使用对应的 ESP-IDF checkout 并 source 其 `export.sh`。v5.5.6 固定到补丁版本；`release/v6.1` 跟随 S31 所需的 6.1 维护分支。其它 IDF 版本（包括更新的补丁版本、release/v6.0 和 master）不在此 CI 验证矩阵内。ESP-IDF 或托管组件的 API 变更可能导致配置/编译失败、运行行为变化，或改变 MicroPython 暴露的 Python API；使用这些版本时，可能需要适配源码、overlay 或依赖，并单独进行硬件验证。

当前 overlay 在 v5.5.6 下提供 SSL/TLS、`cryptolib`、WebSocket、WebREPL、socket events，以及芯片支持时的 `machine.I2S`、PCNT 和 I2C target 模式。这些可选功能在 6.1 构建中仍保持关闭；使用前应检查实际固件。

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

## 问题反馈

ESP-VISION 的所有问题统一提交到 [espressif/esp-vision GitHub Issues](https://github.com/espressif/esp-vision/issues)，包括 IDE、官网、Host Device Core 和固件问题。请先搜索已有问题，再选择下面四类表单之一，支持使用中文或英文填写。

| 分类 | 范围 | 报告入口 |
| --- | --- | --- |
| IDE | 编辑器、项目、界面、预览及烧录流程 | [报告 IDE 问题](https://github.com/espressif/esp-vision/issues/new?template=ide.yml) |
| 官网 | 官网页面、文档、下载、链接及 MCP 知识 | [报告官网问题](https://github.com/espressif/esp-vision/issues/new?template=website.yml) |
| Host Device Core | 主机连接、EV-MUX 会话、RPC、传输及重连 | [报告 Host Device Core 问题](https://github.com/espressif/esp-vision/issues/new?template=host-device-core.yml) |
| 固件 | 编译、启动、崩溃、摄像头、显示、存储及设备端 API | [报告固件问题](https://github.com/espressif/esp-vision/issues/new?template=firmware.yml) |

每类表单均包含环境与版本信息、复现步骤、预期行为、实际行为及相关日志或附件。不确定分类时，请选择最接近的一类，并描述出问题的操作流程。表单源文件位于 [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/)。

## 资源

- [Device Core SDK 与 API 参考](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/device-core/index.html)

- [芯片与开发板支持](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/target-support/index.html)
- [Web IDE](https://ide.vision.espressif.com/)
- [示例](example/)
- [客制化固件功能](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/how-to/customize-firmware.html)
- [方案架构与许可证](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/architecture/index.html)
