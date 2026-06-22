API 参考
========

:link_to_translation:`en:[English]`

ESP-VISION 提供摄像头采集、图像处理、画面显示、视频编码与传输以及 AI 推理等 Python API。开发者还可以直接使用固件中启用的 MicroPython 标准 API，完成硬件控制、网络通信、文件访问、系统管理及通用应用开发。ESP-VISION 在适用范围内兼容常用的 OpenMV 模块名称，包括 ``sensor`` 和 ``image``，并进一步提供 ``display``、``espdl`` 和 ``tflite`` 等模块。

ESP-VISION 模块页面与 ``stubs/`` 下的类型存根保持同步，存根同样可用于 IDE 补全；其可用性在文档构建时根据 ``micropython.cmake`` 自动推导。:doc:`micropython` 页面索引 ESP-VISION 固件从固定 MicroPython 版本沿用的语言、标准库、硬件、网络、文件系统和运行时 API。

.. toctree::
   :maxdepth: 1

   micropython

.. include:: _generated/module-toctrees.rst
