方案架构
========

:link_to_translation:`en:[English]`

ESP-VISION 围绕 MicroPython 固件构建、板级硬件后端、共享平台服务以及面向 Python 的 视觉模块进行组织。代码按是否触及 MicroPython（``mp_obj_t`` / ``py/*.h``）进行分层。

.. figure:: ../../_static/esp-vision-architecture.svg
   :align: center
   :alt: ESP-VISION 分层架构

   ESP-VISION 分层架构总览

分层概览
--------

.. blockdiag::

   blockdiag {
     orientation = portrait;
     default_group_color = none;

     scripts  [label = "MicroPython 脚本\n(example/)"];
     bindings [label = "绑定层 (modules/)\nsensor / image / display / espdl"];
     platform [label = "平台服务\n(platform/)"];
     imlib    [label = "imlib 组件\n(components/imlib)"];
     boards   [label = "板级后端\n(boards/<BOARD>)"];
     mp       [label = "MicroPython + overlay\n(build/, overlay/)"];

     scripts  -> bindings;
     bindings -> platform;
     bindings -> imlib;
     platform -> boards;
     boards   -> mp;
   }

- **绑定层**\ （\ ``modules/``\ ）：即 ``USER_C_MODULES`` 层。四个真实模块 ``image``、 ``sensor``、``display``、``espdl`` 通过 ``MP_REGISTER_MODULE`` 自注册。 ``py_imageio.c`` 提供 ``image.ImageIO`` 类型，``py_helper.c`` 为共享辅助代码。 绑定层只做对象转换与轻量 API 适配，重逻辑放在纯 C 或 ``platform/`` 中。
- **平台服务**\ （\ ``platform/``\ ）：自研的 ESP32 胶水层。``preview.c``\ （通过 USB CDC 的 EVFRAME JPEG 预览）、``display.c``\ （通用显示层）、``sdcard.c``\ （挂载到 ``/sdcard``\ ）、 ``usb_msc.c``\ （通过 TinyUSB MSC 暴露 ``ffat`` 分区）、``jpeg.c``\ （硬件或软件 JPEG）、 ``debug.c``\ ，以及 ``main.c``\ （启动初始化与软复位循环）。
- **imlib 组件**\ （\ ``components/imlib/``\ ）：纯 C 视觉算法，作为以 MIT 维护的 IDF 组件， 源自 OpenMV ``lib/imlib``。
- **板级后端**\ （\ ``boards/<BOARD>/``\ ）：各板配置及真实的相机/显示/SD 卡实现。 P4X 与 S31 使用 ``esp_video``/V4L2，P4X 还使用 PPA；S3 使用 ``esp32-camera``。
- **MicroPython + overlay**\ ：以 MicroPython v1.28.0 为固定基线；项目改动位于 ``overlay/micropython/``\ ，并应用到 ``build/micropython/`` 下的生成副本。

采集到输出的数据流
------------------

.. blockdiag::

   blockdiag {
     orientation = portrait;

     sensor [label = "相机传感器"];
     snap   [label = "sensor.snapshot()"];
     img    [label = "image.Image\n（可复用帧缓冲）"];
     proc   [label = "imlib 处理 /\nespdl 推理"];
     lcd    [label = "display.write()\n-> LCD"];
     prev   [label = "img.flush()\n-> USB CDC 预览"];

     sensor -> snap -> img;
     img -> proc;
     img -> lcd;
     img -> prev;
   }

``sensor.snapshot()`` 将一帧采集到可复用的帧缓冲中，并封装为 ``image.Image``。脚本随后 对图像进行 ``imlib`` 处理或 ``espdl`` 推理，再通过 ``display.write()`` 送往 LCD，或通过 ``img.flush()`` 送往主机预览。

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
     - MicroPython C/C++ 绑定（``sensor``、``image``、``display``、``imageio``、 ``espdl``，以及随芯片启用的 ``h264`` 和 ``rtsp``）。
   * - ``components/``
     - ESP-IDF 组件，包括 OpenMV ``imlib`` 与 ZXing 后端。
   * - ``models/``
     - 运行时从板级存储加载的可选 ``.espdl`` 模型资源。
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
