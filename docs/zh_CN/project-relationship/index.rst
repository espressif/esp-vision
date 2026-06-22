项目关系
========

:link_to_translation:`en:[English]`

本章说明 ESP-VISION 与 MicroPython、OpenMV 及主要第三方组件之间的关系。许可证与分发义务请参阅 :doc:`../license/index`\ 。

ESP-VISION、MicroPython 与 OpenMV
---------------------------------------

三者的关系可以概括为：

- **MicroPython 是语言运行时和固件基础。** ESP-VISION 以固定版本的 MicroPython ESP32 port 为基线，通过 ``overlay/micropython/``、MicroPython 用户 C 模块以及 ESP-IDF 组件加入视觉和板级能力。它不是一个独立的 Python 解释器。
- **OpenMV 是部分视觉实现和 API 设计的上游来源。** ESP-VISION 复用了 OpenMV ``imlib`` 的一部分图像算法，以及部分 ``image`` 绑定与辅助代码，并保留这些文件 原有的许可证和版权声明。
- **ESP-VISION 是面向乐鑫芯片的独立集成项目。** 相机、显示、存储、USB、编解码、 ESP-DL 推理、TFLite Micro 推理和板级适配由 ESP-VISION 结合 ESP-IDF 组件实现。ESP-VISION 不是 OpenMV 固件的分支，也不包含完整的 OpenMV 硬件抽象层、IDE 或全部功能。

因此，``sensor``、``image`` 等模块会尽量保持 OpenMV 风格，方便迁移已有脚本，但 **API 名称相同不表示行为和功能完全相同**。支持范围还会受到芯片、开发板、内存、 硬件外设和编译选项影响。应以本指南的 :doc:`../api-reference/index` 为准。

依赖层次
--------

ESP-VISION 固件中的主要代码层次如下：

.. code-block:: text

   用户脚本
      |
      +-- MicroPython 运行时与 ESP32 port
      |
      +-- ESP-VISION Python 模块
      |      +-- OpenMV 风格的 sensor / image API
      |      +-- display / imageio / espdl / tflite / h264 / rtsp
      |
      +-- 算法与中间件
      |      +-- OpenMV imlib 子集
      |      +-- ulab / ZXing-C++ / ESP-DL / TFLite Micro
      |
      +-- ESP-IDF、托管组件与板级后端

依赖来自三种位置：

- ``lib/`` 中的 Git submodule，例如 MicroPython、``ulab`` 和 ZXing-C++；
- ``components/`` 与部分 ``modules/`` 中随仓库维护的第三方派生代码；
- ``idf_component.yml`` 解析并下载的 ESP Component Registry 组件。具体组件及版本 可能随 ESP-IDF 版本、目标芯片和开发板而变化。

各依赖的版本、本地路径和许可证见 :doc:`../license/index` 清单。
