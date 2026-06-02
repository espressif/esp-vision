# ESP-VISION

[English](README.md) | [简体中文](README_ZH.md)

## 快速开始

环境要求：ESP-IDF release/v5.5（已 source `export.sh`，`idf.py` 可用），以及受支持的 ESP32-P4 或 ESP32-S3 目标板。

```bash
git clone --recursive <本仓库> esp-vision
cd esp-vision
make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor
```

`BOARD` 设置为 `boards/` 和 `overlay/micropython/ports/esp32/boards/` 下的板级包名。

其他常用目标：`make menuconfig`、`make size`、`make erase`、`make clean`、`make distclean`。

顶层 Makefile 在调用 `idf.py` 前执行 `prepare-micropython`：检查 `lib/micropython` 是否处于 MicroPython v1.28.0 commit `e0e9fbb17ed6fd06bb76e266ae554784c9c80804`，随后将 `overlay/micropython/` 应用到该子模块工作区。

## 项目架构

ESP-VISION 是面向 ESP32-P4/ESP32-S3 的 MicroPython 视觉运行时，并配套 VSCode 上位机工具。

### 源码目录

| 路径 | 职责 |
| --- | --- |
| `Makefile` | 顶层构建入口，包装 `idf.py` 并传入 MicroPython ESP32 port 所需参数。 |
| `micropython.cmake` | ESP-VISION 的 MicroPython 集成入口，注册用户模块、平台源码、板级源码、头文件路径和 `ulab`。 |
| `lib/micropython` | MicroPython v1.28.0 子模块，作为上游固定基线。 |
| `lib/ulab` | 固定版本的 `micropython-ulab` 子模块，提供数值计算能力。 |
| `overlay/micropython` | ESP-VISION 的 MicroPython 增量目录，路径结构与 MicroPython 仓库保持一致。 |
| `boards` | 板级包目录，包含每板配置、冻结脚本清单和板级外设实现。 |
| `platform` | 供 Python 模块复用的运行时服务层，包括 camera、preview、storage、display、USB、JPEG 和 debug 支持。 |
| `modules` | 暴露给脚本的 MicroPython C/C++ 绑定层，包括 `sensor`、`image`、`display`、`imageio` 和 `espdl`。 |
| `components/imlib` | ESP-IDF 组件，包含选定的 OpenMV `imlib` 源文件和 ESP32 兼容层。 |
| `models` | 可选 `.espdl` 模型资源，运行时从 `/flash` 或 `/sdcard` 等板端文件系统加载。 |
| `example` | MicroPython 示例脚本，覆盖 camera、preview、storage、display、image processing 和 ESP-DL 使用流程。 |

### MicroPython Overlay

ESP-VISION 以 MicroPython v1.28.0 作为固定上游基线。针对 MicroPython ESP32 port 的项目增量统一维护在 `overlay/micropython/`。

`prepare-micropython` 会将 overlay 应用到 `lib/micropython/`。准备完成后，该子模块中出现 overlay 对应的修改或未跟踪文件属于预期状态。

### 适配新板

板级包分布在两处：MicroPython ESP32 port overlay (`overlay/micropython/ports/esp32/boards/<BOARD>/`) 和 ESP-VISION 主仓库 (`boards/<BOARD>/`)。

**1. MicroPython port 侧** — `overlay/micropython/ports/esp32/boards/<BOARD>/`：

| 文件 | 作用 |
| --- | --- |
| `mpconfigboard.cmake` | IDF target 和 `SDKCONFIG_DEFAULTS` 串联 |
| `mpconfigboard.h` | MicroPython feature flag |
| `sdkconfig.board` | 板级 ESP-IDF Kconfig 覆盖 |
| `partitions-*.csv` | 分区表。 |
| `board.json`、`board.md` | 上游 board manifest 元数据 |

**2. ESP-VISION 侧** — `boards/<BOARD>/`：

| 文件 | 作用 |
| --- | --- |
| `boardconfig.h` | 引脚分配和板级运行时常量。 |
| `imlib_config.h` | OpenMV `imlib` 功能开关。 |
| `manifest.py` | 冻结的 Python 模块。 |
| `camera.c` | 板级 camera 后端实现。 |
| `display.c` | LCD 面板和背光实现。 |
| `sdcard.c` | SD 卡电源和卡检测实现。 |

**3. 构建和烧录**：

```bash
make BOARD=<NEW_BOARD> ESPPORT=/dev/ttyACM0 build flash monitor
```

## 许可证

ESP-VISION 自有代码以 Apache License 2.0 发布。引入的第三方代码保持各文件 SPDX 或许可证头中声明的原始许可证。

| 仓库 | 本地路径 | 用途 | 许可证 |
| --- | --- | --- | --- |
| [MicroPython](https://github.com/micropython/micropython) | `lib/micropython` | MicroPython 运行时及 ESP32 移植基础 | MIT |
| [micropython-ulab](https://github.com/v923z/micropython-ulab) | `lib/ulab` | `ulab` 数值计算模块 | MIT |
| [OpenMV](https://github.com/openmv/openmv) `imlib` MIT 子集，不含下方单独列出的文件 (v4.8.1) | `components/imlib` | 图像处理与绘制算法 | MIT |
| OpenMV `imlib` 中的 AprilTag 算法 | `components/imlib/upstream/apriltag.c` | AprilTag 和矩形检测 | BSD-2-Clause |
| [ZXing-C++](https://github.com/zxing-cpp/zxing-cpp) | `lib/zxing-cpp` | 1D 条码识别后端 | Apache-2.0 |
| [ESP-DL](https://github.com/espressif/esp-dl) | 来自 ESP Component Registry | 模型推理运行时 | MIT |
| [esp_new_jpeg](https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg) | 来自 ESP Component Registry | 软件 JPEG codec 库 | Espressif MIT |
| [esp32-camera](https://github.com/espressif/esp32-camera) | 来自 ESP Component Registry | Camera 驱动库 | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | 外部 SDK | ESP32 构建系统、驱动、JPEG/PPA/Camera 等组件 | Apache-2.0 |
