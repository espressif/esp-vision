项目关系与许可证
================

:link_to_translation:`en:[English]`

本章说明 ESP-VISION 与 MicroPython、OpenMV 及主要第三方组件之间的关系，并解释
源码与固件分发时应如何理解各自的许可证。这里的说明用于帮助开发者定位代码来源，
不构成法律意见；具体义务始终以对应版本的许可证原文和源文件头为准。

ESP-VISION、MicroPython 与 OpenMV
---------------------------------------

三者的关系可以概括为：

- **MicroPython 是语言运行时和固件基础。** ESP-VISION 以固定版本的 MicroPython
  ESP32 port 为基线，通过 ``overlay/micropython/``、MicroPython 用户 C 模块以及
  ESP-IDF 组件加入视觉和板级能力。它不是一个独立的 Python 解释器。
- **OpenMV 是部分视觉实现和 API 设计的上游来源。** ESP-VISION 复用了 OpenMV
  ``imlib`` 的一部分图像算法，以及部分 ``image`` 绑定与辅助代码，并保留这些文件
  原有的许可证和版权声明。
- **ESP-VISION 是面向乐鑫芯片的独立集成项目。** 相机、显示、存储、USB、编解码、
  ESP-DL 推理和板级适配由 ESP-VISION 结合 ESP-IDF 组件实现。ESP-VISION 不是
  OpenMV 固件的分支，也不包含完整的 OpenMV 硬件抽象层、IDE 或全部功能。

因此，``sensor``、``image`` 等模块会尽量保持 OpenMV 风格，方便迁移已有脚本，但
**API 名称相同不表示行为和功能完全相同**。支持范围还会受到芯片、开发板、内存、
硬件外设和编译选项影响。应以本指南的 :doc:`../api-reference/index` 和
:doc:`../examples/index` 为准。

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
      |      +-- display / imageio / espdl / h264 / rtsp
      |
      +-- 算法与中间件
      |      +-- OpenMV imlib 子集
      |      +-- ulab / ZXing-C++ / ESP-DL
      |
      +-- ESP-IDF、托管组件与板级后端

依赖来自三种位置：

- ``lib/`` 中的 Git submodule，例如 MicroPython、``ulab`` 和 ZXing-C++；
- ``components/`` 与部分 ``modules/`` 中随仓库维护的第三方派生代码；
- ``idf_component.yml`` 解析并下载的 ESP Component Registry 组件。具体组件及版本
  可能随 ESP-IDF 版本、目标芯片和开发板而变化。

主要第三方代码
--------------

下表列出与理解项目关系最相关的直接依赖。它不是所有传递依赖的完整清单。

.. list-table::
   :header-rows: 1
   :widths: 18 23 35 24

   * - 项目或代码
     - 本地位置
     - 在 ESP-VISION 中的作用
     - 主要许可证
   * - MicroPython v1.28.0
     - ``lib/micropython``
     - Python 运行时和 ESP32 port 基线
     - MIT
   * - micropython-ulab 6.12
     - ``lib/ulab``
     - 面向 MicroPython 的数组与数值计算
     - MIT
   * - OpenMV ``imlib`` 子集
     - ``components/imlib``
     - 图像表示、绘图和视觉算法
     - 主要为 MIT；存在逐文件例外
   * - OpenMV Python 绑定代码
     - ``modules/py_image*``、``modules/py_helper*`` 等
     - 将部分 OpenMV 风格图像 API 暴露给 MicroPython
     - MIT
   * - AprilTag 实现
     - ``components/imlib/upstream/apriltag.c``
     - AprilTag 与相关检测算法
     - BSD-2-Clause
   * - ZXing-C++ v3.0.2
     - ``lib/zxing-cpp``
     - 条形码识别后端
     - Apache-2.0
   * - ESP-DL
     - ESP Component Registry
     - 神经网络模型推理
     - MIT
   * - ESP-IDF 与其他托管组件
     - 外部 SDK、``managed_components/``
     - 驱动、网络、USB、视频和编解码等能力
     - 按组件或文件分别授权

OpenMV 与 GPL 代码路径
----------------------

OpenMV 上游并非所有可选算法都采用相同许可证。ESP-VISION 在
``micropython.cmake`` 和 ``components/imlib/CMakeLists.txt`` 中设置
``OMV_NO_GPL=1``，不编译 OpenMV 中受 GPL 条件控制的代码路径。这个开关只说明
当前默认构建排除了这些代码，不应被理解为对任意新增文件许可证的自动判定。

修改 ``imlib`` 功能开关、同步 OpenMV 上游代码或加入新算法时，必须逐文件检查
许可证；不要仅根据文件所在目录推断许可证。

如何理解许可证
--------------

ESP-VISION 仓库顶层的 ``LICENSE`` 是 Apache License 2.0，适用于 ESP-VISION
自有代码，但不会覆盖或替换第三方代码的许可证：

- 第三方文件继续由其原始许可证授权，并保留原始版权与许可证头；
- 同一个目录可能包含不同许可证，例如 ``imlib`` 主要为 MIT，而
  ``apriltag.c`` 为 BSD-2-Clause；
- Apache-2.0、MIT 和 BSD 等宽松许可证通常允许组合分发，但仍需履行保留版权、
  许可证文本和声明等条件；
- 某些乐鑫组件使用带产品范围条件的许可证。例如当前 ``esp_new_jpeg`` 许可证文本
  限定用于乐鑫产品，不能只根据名称将其视为无附加条件的通用 MIT；
- 商标权、专利权、模型文件和数据集可能有独立条款，不能从源码许可证自动推导。

发布源码或固件前，至少应检查：

#. 仓库顶层 ``LICENSE``；
#. 每个 Git submodule 的 ``LICENSE``；
#. 所选构建生成的 ``managed_components/*/LICENSE``；
#. 被修改或新增源文件的 SPDX 标识、许可证头和版权声明；
#. 模型、字体、测试图片等非代码资产的来源和使用条款。

增加第三方代码
--------------

向 ESP-VISION 增加第三方包或源文件时：

#. 固定可复现的版本或提交；
#. 逐文件确认许可证及其与项目分发方式的兼容性；
#. 保留上游版权、SPDX 标识、许可证文本和必要的 NOTICE；
#. 在文档或仓库许可证清单中记录来源、版本、本地路径、用途和许可证；
#. 确认构建开关确实排除了不准备分发的代码，而不是仅在文档中声明；
#. 许可证不清楚或存在商业发布约束时，在合入前完成法律审查。
