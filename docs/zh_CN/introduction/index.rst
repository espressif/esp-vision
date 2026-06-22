简介
====

:link_to_translation:`en:[English]`

什么是 ESP-VISION
-----------------

ESP-VISION 是面向乐鑫 SoC 的低代码端侧 AI 与计算机视觉框架，深度整合了摄像头采集、图像处理、视频编解码、网络传输、模型部署和 AI 推理等核心能力，并提供统一标准化的 Python 接口，赋能开发者快速构建集视觉采集、智能识别、画面显示与流媒体传输于一体的边缘应用。

主要特性
--------

- 为受支持的芯片和开发板统一提供摄像头、图像、显示、视频编码、预览与推流 API。
- 提供绘图、滤波、颜色追踪、特征检测、二维码、条码和 AprilTag 等图像处理能力。
- 基于 ESP-DL 提供目标检测、姿态估计和图像分类能力，同时支持通过 TensorFlow Lite Micro 运行 ``.tflite`` 模型。
- 底层高效的 C/C++ 基础组件深度协同芯片的多媒体外设与硬件加速模块，切实保障应用的高效与实时运行性能。
- 可通过 VSCode 主机工具或 Web IDE 进行开发，并使用 ``idf.py`` 管理固件构建。

支持的开发板
------------

ESP-VISION 支持基于 ESP32-P4、ESP32-S3 与 ESP32-S31 的开发板。完整开发板列表以及各芯片的模块和限制见 :doc:`../target-support/index`\ 。

构建与烧录固件请参阅 :doc:`../get-started/index`，整体架构请参阅 :doc:`../architecture/index`。
