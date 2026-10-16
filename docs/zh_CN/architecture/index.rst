方案架构
========

:link_to_translation:`en:[English]`

ESP-VISION 将主机应用与设备固件分开：IDE、Agent、CLI 和自定义工具共用 Device Core，由 SDK 通过开发板支持的 USB CDC、USB Serial/JTAG、UART 或 TCP 实现 EV-MUX v4 主机端。设备固件包含 MicroPython、视觉/推理模块、平台服务和板级后端；固件代码进一步按是否触及 MicroPython（``mp_obj_t`` / ``py/*.h``）分层。

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: ESP-VISION 分层架构

   主机应用 → Device Core → EV-MUX → 设备固件

分层概览
--------

- **主机应用：** IDE 提供编辑/调试界面，Agent 承载自动化流程，CLI 处理终端命令，自定义工具提供自己的应用策略；它们平级使用同一个 SDK。
- **Device Core** （``host/device-core``）：独立版本的 ``@esp-vision/device-core`` npm 包负责协议解析、请求关联、会话、保活与操作生命周期。图中只选有代表性的 API 名称，完整契约见 :doc:`../device-core/api`。
- **EV-MUX v4：** Core 与固件之间的运行时线协议，在 USB CDC、USJ、UART、TCP 上统一帧格式、逻辑通道、所有权与 VM epoch。主机适配器和网络桥只传字节，不另建会话 owner。
- **设备应用：** MicroPython 脚本运行于开发板并调用 Python 绑定；主机断开后，设备应用仍可独立运行。
- **绑定层**\ （\ ``modules/``\ ）：即 ``USER_C_MODULES`` 层。主要模块 ``image``、 ``sensor``、``display``、``espdl`` 和 ``tflite`` 通过 ``MP_REGISTER_MODULE`` 自注册。 ``py_imageio.c`` 提供 ``image.ImageIO`` 类型，``py_helper.c`` 为共享辅助代码。 绑定层只做对象转换与轻量 API 适配，重逻辑放在纯 C 或 ``platform/`` 中。
- **平台服务**\ （\ ``platform/``\ ）：自研的 ESP32 胶水层。``ev_channel.c`` / ``ev_mux.c`` / ``ev_control_transport.c`` / ``ev_stdio.c``\ （EV-MUX / EV-ATP 底层传输）、``preview.c``\ （EV-MUX JPEG 预览）、``display.c``\ （通用显示层）、``sdcard.c``\ （挂载到 ``/sdcard``\ ）、 ``usb_msc.c``\ （通过 TinyUSB MSC 暴露 ``ffat`` 分区）、``usb_auto_download.c``\ （S3/P4 OTG CDC 复位/下载协议）、``jpeg.c``\ （硬件或软件 JPEG）、 ``debug.c``\ ，以及 ``main.c``\ （启动初始化与软复位循环）。
- **imlib 组件**\ （\ ``components/imlib/``\ ）：纯 C 视觉算法，作为以 MIT 维护的 IDF 组件， 源自 OpenMV ``lib/imlib``。
- **板级后端**\ （\ ``boards/<BOARD>/``\ ）：各板配置及真实的相机/显示/SD 卡实现。 P4X 与 S31 使用 ``esp_video``/V4L2，P4X 还使用 PPA；S3 使用 ``esp32-camera``。
- **MicroPython + overlay**\ ：以 MicroPython v1.28.0 为固定基线；项目改动位于 ``overlay/micropython/``\ ，并应用到 ``build/micropython/`` 下的生成副本。

宿主提供的字节传输适配器承接 SDK 协议之下的物理 I/O。局域网 DNS-SD 发现属于连接前的枚举路径；ROM 固件烧写使用独立 loader 生命周期，不通过 EV-MUX 传输应用镜像。固件与 SDK 版本保持独立；主机接入及 API 文档见 :doc:`../device-core/index`。

采集到输出的数据流
------------------

.. blockdiag::

   blockdiag {
     orientation = portrait;

     sensor [label = "相机传感器"];
     snap   [label = "sensor.snapshot()"];
     img    [label = "image.Image\n（可复用帧缓冲）"];
     proc   [label = "imlib 处理 /\nAI 推理"];
     lcd    [label = "display.write()\n-> LCD"];
     prev   [label = "img.flush()\n-> EV-MUX 预览"];

     sensor -> snap -> img;
     img -> proc;
     img -> lcd;
     img -> prev;
   }

``sensor.snapshot()`` 将一帧采集到可复用的帧缓冲中，并封装为 ``image.Image``。脚本随后 对图像进行 ``imlib`` 处理、ESP-DL 推理或 TFLite Micro 推理，再通过 ``display.write()`` 送往 LCD，或通过 ``img.flush()`` 送往主机预览。

通信子系统
----------

主机通信采用 EV-MUX v4，会话同时持有 user/debug 两个 stream，逻辑通道复用在当前 owner 的 USJ、USB、UART 或 TCP link 上。MicroPython HAL、C stdio、IDF 日志和 JPEG preview 分别接入对应通道。

完整的输出层级、物理接口、会话接管、帧格式、文件/Python/图像协议和验收说明已独立为 :doc:`../communication/index`，避免在工程架构、Agent 指南和主机适配页重复维护。当前 MicroPython 异常与 sys.stderr 的具体输出路径见 :doc:`../communication/architecture`。

源码结构
--------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - 路径
     - 职责
   * - ``idf_ext.py``
     - 仓库根目录下板级感知的 ``idf.py`` 扩展。
   * - ``micropython.cmake``
     - 集成枢纽：注册用户模块、平台与板级源文件、include 路径、条件性 ``zxing`` 与 ``ulab``。
   * - ``lib/``
     - 固定版本的第三方子模块（MicroPython、``ulab``、ZXing-C++）。
   * - ``overlay/micropython/``
     - 采用 MicroPython 路径布局的 ESP-VISION MicroPython 增量。
   * - ``boards/``
     - 各板配置、冻结清单与板级外设后端。
   * - ``platform/``
     - 共享运行时服务（相机、预览、存储、显示、USB、JPEG）。
   * - ``modules/``
     - MicroPython C/C++ 绑定（``sensor``、``image``、``display``、``imageio``、 ``espdl``、``tflite``，以及随芯片启用的 ``h264`` 和 ``rtsp``）。
   * - ``components/``
     - ESP-IDF 组件，包括 OpenMV ``imlib`` 与 ZXing 后端。
   * - ``models/``
     - 运行时从板级存储加载的可选模型资源。
   * - ``example/``
     - MicroPython 示例脚本。
   * - ``stubs/``
     - 描述 C 模块的 ``.pyi`` 类型存根。

板卡的组成
----------

一块开发板的定义集中在单棵目录树 ``boards/<BOARD>/`` 中：

- ESP-VISION 侧（顶层）：``boardconfig.h``、``imlib_config.h``、 ``manifest.py``，以及可选的 ``camera.c`` / ``display.c`` / ``sdcard.c``。
- MicroPython 移植侧（``boards/<BOARD>/port/``）：IDF 目标、sdkconfig、分区表、USB 字符串。构建时会将该子目录投射到生成的 MicroPython 副本的 ``ports/esp32/boards/<BOARD>/``。

完整步骤请参阅 :doc:`../how-to/add-board`。

随芯片变化的源码
----------------

``micropython.cmake`` 根据 ``IDF_TARGET`` 和板级配置选择模块。ESP32-P4 构建包含 ``h264`` 与 ``rtsp``；当前 P4 板级配置还会启用 ZXing-C++ 条形码后端。最终公开 API 矩阵见 :doc:`../target-support/index`\ 。

MicroPython Overlay
-------------------

ESP-VISION 以 MicroPython v1.28.0 作为固定上游基线。ESP32 port 的项目增量维护在 ``overlay/micropython/`` 下。``prepare-micropython`` 构建步骤会将其应用到 ``build/micropython/idf<ESP_IDF_VERSION>/micropython/`` 下的生成副本； ``lib/micropython`` 子模块保持为干净的上游参考。

ESP-VISION 与上游项目的关系见 :doc:`../project-relationship/index`；各组件的许可证见 :doc:`../license/index`\ 。
