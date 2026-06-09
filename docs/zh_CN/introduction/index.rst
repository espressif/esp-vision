简介
====

:link_to_translation:`en:[English]`

什么是 ESP-VISION
-----------------

ESP-VISION 是面向 ESP32 平台的 MicroPython 视觉运行时。它在 MicroPython ESP32
官方移植的基础上叠加 ESP-VISION overlay、一组暴露给 Python 的 C 模块、自研的平台层，
以及作为视觉组件的 OpenMV ``imlib``，最终构建出定制固件。

主要特性
--------

- ``sensor``、``image``、``display`` 等 Python API，用于相机采集、图像处理、预览与
  LCD 输出。
- 通用的板级运行时服务，覆盖相机、显示、存储、USB 预览、JPEG 以及板级外设。
- 部分基于 OpenMV ``imlib`` 的视觉算法，涵盖绘图、滤波、颜色追踪、特征检测、二维码、
  条形码与 AprilTag。
- 基于 ESP-DL 的模型推理辅助类，支持目标检测、姿态检测与图像分类。
- 可通过 VSCode 主机工具或 Web IDE 进行开发，并使用 ``make`` 或 ``idf.py`` 构建固件。

支持的开发板
------------

ESP-VISION 面向 ESP32-P4、ESP32-S3 与 ESP32-S31 开发板。当前支持的板级包包括
``ESP32_P4X_EYE``、``ESP32_P4X_FUNCTION_EV_BOARD``、``ESP32_S3_EYE`` 与
``ESP32_S31_KORVO``，其中 ``TEMPLATE`` 用于新板卡的初始适配。各 target 的模块和
限制见 :doc:`../target-support/index`\ 。

构建与烧录固件请参阅 :doc:`../get-started/index`，整体架构请参阅
:doc:`../architecture/index`。
