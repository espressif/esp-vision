客制化固件功能
==============

:link_to_translation:`en:[English]`

ESP-VISION 将面向应用的模块、图像算法、标准 MicroPython 功能、冻结 Python 代码和板级服务拆分为相互独立的配置层。产品固件既可以移除不需要的能力，以减少 Flash、RAM、依赖项和攻击面，也可以增加产品专用模块，而无需重写摄像头与媒体处理链路。

确定客制化范围
--------------

修改配置前，应先确定变更是面向所有 ESP-VISION 固件，还是仅面向某块开发板。修改根目录 ``micropython.cmake`` 会影响所有满足对应芯片条件的开发板；修改 ``boards/<BOARD>/``（包括其中的 MicroPython 移植文件 ``boards/<BOARD>/port/``）仅影响对应开发板。对于产品变体，应按照 :doc:`add-board` 创建独立板级包，而不是直接修改共享的开发板配置。

客制化 ESP-VISION Python 模块
-----------------------------

``micropython.cmake`` 中的 ``ESP_VISION_MODULE_SOURCES`` 列表决定暴露给 Python 的 C/C++ 绑定。移除源码时，应同时处理其依赖的辅助源码；增加源码时可参考 :doc:`add-python-module`。仅适用于部分芯片的模块应放在对应的 ``IDF_TARGET`` 条件中，例如 H.264 与 RTSP 当前仅为 ``esp32p4`` 启用。

.. code-block:: cmake

   set(ESP_VISION_MODULE_SOURCES
       ${ESP_VISION_ROOT}/modules/py_display.c
       ${ESP_VISION_ROOT}/modules/py_image.c
       ${ESP_VISION_ROOT}/modules/py_imageio.c
       ${ESP_VISION_ROOT}/modules/py_helper.c
       ${ESP_VISION_ROOT}/modules/py_sensor.c
   )

移除绑定源码不会自动移除其背后的所有平台服务或托管组件。修改源码列表后，还需检查 ``target_sources``、``target_link_libraries``、组件清单和最终 size 报告，确认不需要的依赖已不再链接。

客制化图像算法
--------------

每块开发板的 ``boards/<BOARD>/imlib_config.h`` 用于选择可选的 ``imlib`` 算法。删除某个 ``IMLIB_ENABLE_*`` 定义可排除不使用的算法，增加受支持的定义可启用其他算法。常见功能组包括滤波、几何检测、二维码和 AprilTag。

.. code-block:: c

   #define IMLIB_ENABLE_MEAN
   #define IMLIB_ENABLE_GAUSSIAN
   #define IMLIB_ENABLE_QRCODES
   #define IMLIB_ENABLE_APRILTAGS

部分 Python 方法在后端关闭后仍会保留在绑定层中，并在调用时抛出异常。应确保 API 文档、示例和自动生成的开发板支持表与所选算法保持一致。未经许可证审查，不应移除 ``OMV_NO_GPL``。

客制化标准 MicroPython 功能
---------------------------

使用 ``boards/<BOARD>/port/mpconfigboard.h`` 覆盖单块开发板的标准 MicroPython 功能宏，例如网络、Bluetooth、ESP-NOW、ADC、SD 卡、USB，以及其他 ``MICROPY_PY_*`` 和 ``MICROPY_HW_*`` 选项。CMake 级选项和板级 sdkconfig 链则在 ``mpconfigboard.cmake`` 中配置。

.. code-block:: c

   #define MICROPY_PY_BLUETOOTH (0)
   #define MICROPY_PY_ESPNOW (0)
   #define MICROPY_PY_NETWORK_WLAN (1)

这些宏可能依赖 ESP-IDF 版本与 SoC 能力宏。关闭 Python 功能后，可能还需移除对应的 ESP-IDF 配置或托管依赖，才能获得可观测的固件体积缩减。

配置内存放置策略
----------------

带 PSRAM 的开发板通过 ``boards/<BOARD>/port/`` 下的 sdkconfig 配置链设置分配边界。``CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`` 是 ESP-IDF 默认分配器和 ESP-VISION 自动分配策略使用的字节阈值：不大于该阈值的分配优先使用内部 RAM，更大的分配优先使用 PSRAM。``CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`` 则单独为驱动和其他必须使用内部 RAM 的分配预留内存。需要调整内存分配比例时，应修改开发板的 sdkconfig 默认值。

大型图像、模型、帧和临时缓冲区在所需能力允许时优先使用 PSRAM。调用方要求直接 DMA 访问或可执行访问时，分配回退仍保留该能力。P4X_EYE 的 SPI 显示也可以把普通 8-bit framebuffer 作为绘制源，因为 LCD 传输层会将其分块搬运到有界 DMA 传输缓冲区；这是明确的复制路径，不会放宽直接 DMA 的要求。没有 PSRAM 的开发板会在能力允许时使用内部内存。

客制化冻结 Python 代码
----------------------

开发板的 ``boards/<BOARD>/manifest.py`` 控制冻结到固件中的 Python 模块。可根据 MicroPython manifest 机制使用 ``freeze()``、``module()``、``package()`` 或 ``include()`` 增加产品启动代码、库或软件包；不需要某个冻结模块时则删除对应条目。

.. code-block:: python

   freeze("$(PORT_DIR)/modules")
   freeze("$(ESP_VISION_ROOT)/modules", "py_inisetup.py")
   freeze("$(ESP_VISION_ROOT)/boards/<BOARD>", "board_inisetup.py")
   include("$(MPY_DIR)/extmod/asyncio")

冻结代码可提升部署一致性并确保启动阶段可用，但会占用固件 Flash。开发期间或产品部署后仍需替换的文件，应保留在根文件系统中（例如 ``/lib`` 下的软件包）或 ``/sdcard`` 中。

客制化板级服务与可选组件
------------------------

板级摄像头、显示和 SD 卡实现位于 ``boards/<BOARD>/``。存在 ``camera.c``、``display.c`` 和 ``sdcard.c`` 时，``micropython.cmake`` 会自动选用。板级 CMake 开关应放在 ``boards/<BOARD>/board.cmake`` 中，例如当前 P4 开发板通过 ``ESP_VISION_ENABLE_BARCODE`` 启用可选的 ZXing 条码后端。

.. code-block:: cmake

   set(ESP_VISION_ENABLE_BARCODE OFF)

增加组件时，应注册其源码或 IDF 组件，并将其链接到 ``usermod_esp_vision_platform``；移除组件时，应一次性移除源码、头文件路径、编译定义、链接依赖和组件清单条目。

构建与验证
----------

修改 CMake、sdkconfig、manifest 或功能宏后，应重新配置并构建所选开发板：

.. code-block:: bash

   idf.py --board <BOARD> reconfigure
   idf.py --board <BOARD> build
   idf.py --board <BOARD> size

在 REPL 中验证模块导入和受影响的 API，运行相关示例，并对比修改前后的 size 报告。还应重新构建文档，因为模块导航和开发板能力摘要会根据固件配置自动生成。
