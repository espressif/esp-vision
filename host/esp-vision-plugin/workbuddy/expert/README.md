# ESP-Vision AI视觉

ESP-VISION 是乐鑫开源的低代码端侧 AI 与计算机视觉项目，基于 MicroPython 提供统一的 Python 接口，帮助开发者在 ESP32 系列芯片上构建摄像头采集、图像处理、AI 推理、显示和视频传输应用。

通过 WorkBuddy 专家，你可以查询 ESP-VISION 官方文档与 API、了解开发板能力，并在指定设备后检查设备、部署 MicroPython 程序和验证运行结果。

## 支持的硬件

项目提供基于 ESP32-P4、ESP32-S3 和 ESP32-S31 的开发板配置，包括 ESP32-P4X-EYE、ESP32-P4X-Function-EV-Board、ESP32-P4X-Vision、ESP32-S3-EYE、AtomS3R-M12，以及 ESP32-S31 Chatbot、Korvo 和 Mosaico。具体功能取决于开发板硬件和固件配置，请查看[支持的开发板](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/target-support/index.html)。

如果你的开发板尚未列出，可以参考开源仓库中的 `boards/TEMPLATE` 和已有开发板配置，适配摄像头、显示、存储等硬件并自行构建固件。新硬件需要根据实际配置完成适配和验证。

## 开始使用

- 在 WorkBuddy 中连接官方知识服务，即可查询文档；知识查询需要网络，但不需要连接设备。
- 设备操作需要支持本地命令执行的 WorkBuddy 桌面环境和 Node.js 22 或更高版本。串口连接使用随包 npm 原生绑定，烧录使用共享 esptool-js 后端，无需主机 Python。设备使用还需要串口权限或可用的 TCP 网络。
- 可以尝试：“查询我的开发板支持哪些 ESP-VISION 视觉 API”“检查我指定设备的固件和连接状态”“帮我部署并验证一个视觉应用”。设备操作前请明确指定目标设备。

## 了解更多

- [ESP-VISION 官网](https://vision.espressif.com/)
- [开源代码与开发板配置](https://github.com/espressif/esp-vision)
- [中文文档](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [Web IDE](https://ide.vision.espressif.com/)

## English

ESP-VISION is an open-source, low-code edge AI and computer vision project from Espressif. Built on MicroPython, it provides Python APIs for camera capture, image processing, AI inference, display and video applications on supported ESP32 hardware. This WorkBuddy Expert helps you explore official documentation, inspect a device you select, deploy MicroPython programs and verify their results.

Board configurations cover ESP32-P4, ESP32-S3 and ESP32-S31, including ESP32-P4X-EYE, ESP32-P4X-Function-EV-Board, ESP32-P4X-Vision, ESP32-S3-EYE, AtomS3R-M12 and ESP32-S31 Chatbot, Korvo and Mosaico. Features vary by board and firmware; see the [supported boards](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/target-support/index.html). To add another board, use `boards/TEMPLATE` and existing board configurations in the [open-source repository](https://github.com/espressif/esp-vision) as a starting point, adapt its hardware and build and validate the firmware.

Connect the official knowledge service in WorkBuddy for documentation queries. Device operations require a WorkBuddy desktop environment with local execution and Node.js 22 or newer; serial access uses bundled npm native bindings and flashing uses shared esptool-js, without host Python. Select the device explicitly before running device operations.

Visit the [ESP-VISION website](https://vision.espressif.com/), [documentation](https://docs.espressif.com/projects/esp-vision/en/latest/) or [Web IDE](https://ide.vision.espressif.com/). License information for the project and third-party components is included with the Expert and in the repository.
