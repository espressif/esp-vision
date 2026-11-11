许可证
======

:link_to_translation:`en:[English]`

本章是 ESP-VISION 许可证相关说明的唯一参考。这里的内容用于帮助开发者理解代码来源，不构成法律意见；具体义务始终以对应版本的许可证原文和源文件头为准。

ESP-VISION 自有代码（``platform/``、大部分 ``modules/``、``boards/`` 以及构建文件）以 Apache License 2.0 发布，记录在仓库顶层的 ``LICENSE`` 中。引入的第三方代码保持其源文件、SPDX 标识或上游许可证声明的原始许可证；仓库顶层 ``LICENSE`` 不会覆盖或替换这些许可证。

许可证清单
----------

下表列出与分发最相关的直接依赖。它不是所有传递依赖的完整清单。

.. list-table::
   :header-rows: 1
   :widths: 22 26 32 20

   * - 项目或代码
     - 本地路径
     - 在 ESP-VISION 中的作用
     - 许可证
   * - `esptool-js 0.7.1 <https://github.com/espressif/esptool-js/tree/v0.7.1>`__
     - ``host/device-core/package.json`` 依赖，由 ``src/rom_loader.js`` 延迟加载
     - 浏览器 Web Serial 与 Node 共用的官方 ROM 后端；上游 ``src/targets/stub_flasher/README.md`` 标明 stub 来源为 esp-flasher-stub v1.2.2（set 2）
     - Apache-2.0（安装包 ``LICENSE``）；stub 二进制双许可 Apache-2.0/MIT。作为依赖安装，实例级适配不修改上游源文件。间接依赖 pako 保留其 MIT/Zlib 声明，atob-lite 保留 MIT。
   * - @serialport/bindings-cpp 13.0.0; Koffi 2.16.3
     - ``host/device-core/package.json`` optional dependencies; CLI/plugin native bundles
     - 原生串口与 Linux USJ 控制线；原版 npm 预编译二进制和加载器
     - MIT; original package licenses and Koffi Node API/cnoke notices retained in portable bundles
   * - Device Core 主机 SDK
     - ``host/device-core``
     - EV-MUX v4 主机通信、Python、资源文件、发现及图像接入
     - Apache-2.0（``host/device-core/LICENSE``）；历史 MIT 声明保留于 ``NOTICE``
   * - 设备端相机/LCD 测试片段
     - ``host/device-core/tests/fixtures/device-checks.py``
     - 从固件测试迁入的真机验收函数，不包含在 npm 包中
     - Apache-2.0，保留原版权声明
   * - `sync-pr-to-gitlab <https://github.com/espressif/sync-pr-to-gitlab>`__ (6ea5db6)
     - ``.github/actions/sync-pr-to-gitlab``
     - GitHub PR 到 GitLab MR 的同步，移除 Jira 专用元数据；保留上游 Python 版权和许可证声明
     - Apache-2.0
   * - `MicroPython <https://github.com/micropython/micropython>`__ v1.28.0
     - ``lib/micropython``
     - Python 运行时与 ESP32 port 基线
     - MIT
   * - MicroPython 执行与线程适配
     - ``overlay/micropython/shared/runtime/pyexec.c``、``overlay/micropython/ports/esp32/mpthreadport.c``
     - 基于固定上游文件捕获和继承会话来源，保留原版权和许可证头
     - MIT
   * - MicroPython 复合 USB 描述符适配
     - ``overlay/micropython/shared/tinyusb/mp_usbd_descriptor.c`` （v1.28.0 基线）
     - CDC/MSC/UVC 描述符及速率适配，保留上游许可证头
     - MIT
   * - OV2710 定时器生命周期适配
     - ``tools/prepare_ov2710.py``、``platform/ov2710_lifecycle.inc`` 与原样测试源 ``tools/tests/fixtures/ov2710/upstream.c``；esp_cam_sensor 2.4.0，提交 ``8fc931636ffa937c89cab53b53f4ce380190c1a4``
     - 构建时校验并复制 ``sensors/ov2710/ov2710.c``（SHA-256 ``4a9af395151527b156b1d685c39b298502490f33a8391a641a51ac72b3bc8876``）；确认回调停止后才释放 SCCB，保留原始文件头，不修改托管组件
     - Apache-2.0
   * - TinyUSB UVC 传输适配
     - ``overlay/micropython/shared/tinyusb/esp_vision_video_transfer.inc``、``tools/tests/tinyusb_video_stubs/upstream_transfer.c``；espressif/tinyusb 0.19.0~3，提交 ``ab4a1817907ed865b10a712b5e03e5e0a9902df5``
     - 构建时生成经检查的 ``src/class/video/video_device.c`` 副本；终止失败或短包 ISO 帧，单独报告失败完成，并保留原版权和许可证头
     - MIT
   * - `micropython-ulab <https://github.com/v923z/micropython-ulab>`__ 6.12
     - ``lib/ulab``
     - 数组与数值计算
     - MIT
   * - `OpenMV v5.0.0 <https://github.com/openmv/openmv/tree/v5.0.0>`__ ``imlib`` 子集
     - ``components/imlib``
     - 图像处理与绘制算法
     - MIT，部分文件单独授权
   * - OpenMV Python 绑定代码
     - ``modules/py_image*``、``modules/py_helper*``
     - OpenMV 风格的 ``image`` API 绑定
     - MIT
   * - OpenMV 软件 JPEG 适配
     - ``components/imlib/upstream/jpege.c``
     - 将量化器缓存初值设为无效哨兵，确保首次以质量 100 编码也会初始化量化表；保留 OpenMV 原始 MIT 声明
     - MIT
   * - OpenMV AprilTag 实现
     - ``components/imlib/upstream/apriltag.c``
     - AprilTag 与矩形检测
     - BSD-2-Clause
   * - `ZXing-C++ <https://github.com/zxing-cpp/zxing-cpp>`__ v3.0.2
     - ``lib/zxing-cpp``
     - 一维条码识别后端
     - Apache-2.0
   * - `ESP-DL <https://github.com/espressif/esp-dl>`__
     - Component Registry
     - 模型推理运行时
     - MIT
   * - `TensorFlow Lite Micro / esp-tflite-micro <https://github.com/espressif/esp-tflite-micro>`__
     - Component Registry
     - TensorFlow Lite Micro 推理运行时
     - Apache-2.0
   * - `esp_new_jpeg <https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg>`__
     - Component Registry
     - 软件 JPEG 编解码
     - Espressif MIT（限乐鑫产品）
   * - `esp32-camera <https://github.com/espressif/esp32-camera>`__
     - Component Registry
     - 相机驱动
     - Apache-2.0
   * - `ESP-IDF <https://github.com/espressif/esp-idf>`__
     - 外部 SDK
     - 构建系统、驱动和媒体组件
     - Apache-2.0
   * - `NanoTrack V2 预训练模型 <https://github.com/HonglinChu/SiamTrackers/tree/248663fde6bf7c40190cf10ee396d5662919ecd3/NanoTrack/models/nanotrackv2>`__
     - ``models/nanotrack_v2``
     - 本地量化的单目标追踪模型资源
     - 上游 NanoTrack 提供 Apache-2.0 文本；权重独立条款尚未核实，见目录内 README 和来源清单

如何理解许可证
--------------

仓库顶层的 ``LICENSE`` 适用于 ESP-VISION 自有代码，但不会覆盖第三方许可证：

- 第三方文件继续由其原始许可证授权，并保留原始版权与许可证头；
- 同一个目录可能包含不同许可证，例如 ``imlib`` 主要为 MIT，而 ``apriltag.c`` 为 BSD-2-Clause；
- Apache-2.0、MIT 和 BSD 等宽松许可证通常允许组合分发，但仍需履行保留版权、许可证文本和声明等条件；
- 某些乐鑫组件使用带产品范围条件的许可证。例如当前 ``esp_new_jpeg`` 许可证文本限定用于乐鑫产品，不能只根据名称将其视为无附加条件的通用 MIT；
- 商标权、专利权、模型文件和数据集可能有独立条款，不能从源码许可证自动推导。

发布源码或固件前，至少应检查：

#. 仓库顶层 ``LICENSE``；
#. 每个 Git submodule 的 ``LICENSE``；
#. 所选构建生成的 ``managed_components/*/LICENSE``；
#. 被修改或新增源文件的 SPDX 标识、许可证头和版权声明；
#. 模型、字体、测试图片等非代码资产的来源和使用条款。

OpenMV 与 GPL 代码路径
----------------------

OpenMV 上游并非所有可选算法都采用相同许可证。ESP-VISION 在 ``micropython.cmake`` 和 ``components/imlib/CMakeLists.txt`` 中设置 ``OMV_NO_GPL=1``，不编译 OpenMV 中受 GPL 条件控制的代码路径。这个开关只说明当前默认构建排除了这些代码，不应被理解为对任意新增文件许可证的自动判定。

修改 ``imlib`` 功能开关、同步 OpenMV 上游代码或加入新算法时，必须逐文件检查许可证；不要仅根据文件所在目录推断许可证。

增加第三方代码
--------------

向 ESP-VISION 增加第三方包或源文件时：

#. 固定可复现的版本或提交；
#. 逐文件确认许可证及其与项目分发方式的兼容性；
#. 保留上游版权、SPDX 标识、许可证文本和必要的 NOTICE；
#. 在本清单中记录来源、版本、本地路径、用途和许可证；
#. 确认构建开关确实排除了不准备分发的代码，而不是仅在文档中声明；
#. 许可证不清楚或存在商业发布约束时，在合入前完成法律审查。

二进制上传原生测试通过 PSA 测试替身使用系统 OpenSSL 3 libcrypto（Apache-2.0）作为 SHA-256 对照；仅用于主机测试，不链接到固件，也不随 npm 分发。固件使用 ESP-IDF PSA Crypto。
