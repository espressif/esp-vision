芯片与开发板支持
================

:link_to_translation:`en:[English]`

ESP-VISION 文档按支持的芯片分别构建。请选择开发板固件所使用的芯片，使文档中的模块和 API 与默认固件保持一致。

支持的开发板
------------

.. note::

   当前标准 ESP32-P4 预编译固件支持芯片版本 v3.1–v3.99。这里指 esptool 报告的芯片版本，不是开发板或模组丝印版本。烧录前须核对所选镜像的实际版本范围；仅凭板名或笼统的 v3.x 标记不足以确认兼容。

   此处不提供 ESP32-P4 v1.3 的预编译固件。有需要的用户须自行编译并验证：选择对应板型，在 menuconfig 中启用 ``CONFIG_ESP32P4_SELECTS_REV_LESS_V3``，选择与实际芯片兼容的最低版本，并重新编译 bootloader 和应用。IDF 的 v3 之前版本与 v3.x 目标互不兼容，不能强刷 v3.x 镜像或修改镜像版本头绕过检查。

.. include:: _generated/boards.rst

芯片能力与板级能力
------------------

所选芯片控制芯片级源文件选择。例如，只有 ``IDF_TARGET`` 为 ``esp32p4`` 时， ``micropython.cmake`` 才会加入 ``h264`` 和 ``rtsp``。板级配置还可以通过 ``boards/<BOARD>/board.cmake`` 进一步启用或关闭 ZXing-C++ 条形码后端等功能。

API 导航和芯片相关的符号条件会根据这些构建文件生成。因此， ``micropython.cmake``、 ``boards/*/port/mpconfigboard.cmake`` 和各开发板的 ``board.cmake`` 是 ESP-VISION API 可用性的权威来源。

标准 MicroPython 功能
---------------------

标准 MicroPython 功能采用另一条配置路径：全局 ``overlay/micropython/ports/esp32/mpconfigport.h`` 与各开发板的 ``mpconfigboard.h``。上表会索引这些文件中显式启用的基础能力，其他条件还可能取决于芯片能力和固件配置。

当前开发板头文件也都关闭了 Bluetooth 和 ESP-NOW；S31 开发板还关闭了 ``machine.ADC`` 和 ``machine.ADCBlock``。开发板仍需具备相应网络硬件和固件配置， ``network.WLAN`` 才能建立连接。

这些条件并非只由芯片决定，因此芯片选择器会过滤 ESP-VISION API 参考，但不代表标准 MicroPython 模块的完整清单。构建环境兼容性说明统一维护在项目 README 中。

芯片文档描述当前开发板配置共同采用的默认能力。自定义开发板或修改编译选项后的固件可能与已发布芯片页面不同；发生差异时应以固件配置为准。

WebREPL 与 EV-MUX 网络控制
--------------------------

当前 ESP-VISION EV-MUX 运行态不支持传统 MicroPython WebREPL，ESP-IDF v5.5.6 构建也不例外。部分构建启用了 WebSocket 与内部 ``_webrepl`` 组件。ESP32_P4X_EYE 等部分板型未包含上游 ``webrepl`` Python 服务，ESP32_S3_EYE 的 manifest 则包含该服务；服务是否存在以及这些编译选项均不代表与 EV-MUX 的终端或文件传输兼容。

EV-MUX 的 Python 输入、输出与文件操作受唯一 owner 的连接、租约和 VM 代次校验约束。传统 WebREPL 的 ``dupterm`` 服务未实现此会话约定，因此不支持通过补装 Python 包或转发裸终端字节来绕过它。网络控制请使用 :doc:`../device-core/api` 的 TCP 传输。保留的 ``example/07-Network/00-WebREPL/webrepl.py`` 入口会在改变 Wi-Fi 或打开监听端口前退出，并说明这一限制。
