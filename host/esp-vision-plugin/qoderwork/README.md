# ESP-Vision 嵌入式 AI 视觉开发

ESP-VISION 是乐鑫开源的低代码端侧 AI 与计算机视觉项目，基于 MicroPython 提供统一的 Python 接口，帮助开发者在 ESP32 系列芯片上构建摄像头采集、图像处理、AI 推理、显示和视频传输应用。

通过本插件，你可以在 QoderWork 中查询 ESP-VISION 官方文档与 API，了解开发板能力，并在指定设备后使用随包工具检查设备、部署 MicroPython 程序和验证运行结果。

## 支持的硬件

项目提供基于 ESP32-P4、ESP32-S3 和 ESP32-S31 的开发板配置，包括 ESP32-P4X-EYE、ESP32-P4X-Function-EV-Board、ESP32-P4X-Vision、ESP32-S3-EYE、AtomS3R-M12，以及 ESP32-S31 Chatbot、Korvo 和 Mosaico。具体功能取决于开发板硬件和固件配置，请查看[支持的开发板](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/target-support/index.html)。

如果你的开发板尚未列出，可以参考开源仓库中的 `boards/TEMPLATE` 和已有开发板配置，适配相应的摄像头、显示、存储等硬件并自行构建固件。新硬件需要根据实际配置完成适配和验证。

## 开始使用

- 安装后按 QoderWork 提示配置官方知识连接：`https://mcp.vision.espressif.com/mcp`。该地址用于查询公开文档，不需要设备地址或凭证。
- 设备操作需要 Node.js 22 或更高版本，以及允许本地执行命令的 QoderWork 环境；连接设备还需串口权限或可用的 TCP 网络。
- 可以从这些问题开始：“ESP32-P4X-EYE 支持哪些摄像头 API？”“帮我检查我指定的开发板状态。”“帮我编写并部署一个摄像头预览程序。”设备操作前请明确指定目标设备。

## 了解更多

- [ESP-VISION 官网](https://vision.espressif.com/)
- [开源代码与开发板配置](https://github.com/espressif/esp-vision)
- [中文文档](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/)
- [Web IDE](https://ide.vision.espressif.com/)

项目及第三方组件的许可信息随插件提供，也可在开源仓库中查看。
