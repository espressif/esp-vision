物理接口、连接代次与构建配置
============================

:link_to_translation:`en:[English]`

.. warning::

   TCP 控制接口用于隔离、可信本地网络中的无线开发和调试，不提供对端身份认证或传输加密。能够访问该接口的任意客户端均可取得空闲会话并执行文件上传、脚本运行等开发操作；会话所有权及 CRC 校验不提供操作者身份认证。启用 TCP 传输的开发板在已启用的后台网络服务上线后会自动开放监听，绑定 STA IPv4 地址并不限制客户端必须位于同一子网。请勿将该接口暴露到互联网或不可信网络。不需要该接口时，请在所选开发板的 ``boardconfig.h`` 中将 ``ESP_VISION_ENABLE_TCP_TRANSPORT`` 设为 ``0`` 并重新构建。使用定位、赏金范围及私下报告渠道详见仓库的 `安全政策 <https://github.com/espressif/esp-vision/blob/master/SECURITY.md>`_。

sink 类型与实际 link
--------------------

``capabilities.sinks`` 表示固件编译支持的后端及当前状态。sink 名称是 ``usj``、``usb``、``uart``、``tcp``，不是操作系统的 ``/dev/ttyACM0``，也不是 channel。``usb_otg_cdc`` 表示硬件 kind；对外端点 ID 仍是 ``usb``，不能用 ``cdc`` 或 ``usb_otg_cdc`` 替代。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     host [label = "IDE / Agent / CLI / custom tools"];
     core [label = "Device Core"];
     protocol [label = "EV-MUX v4"];
     usj [label = "usj\nUSB Serial/JTAG"];
     usb [label = "usb\nTinyUSB CDC"];
     uart [label = "uart\nserial pins + adapter"];
     tcp [label = "tcp\nSTA IPv4 :3232"];
     owner [label = "One acquired link\nowns user + debug"];
     host -> core -> protocol;
     protocol -> usj; protocol -> usb; protocol -> uart; protocol -> tcp;
     usj -> owner; usb -> owner; uart -> owner; tcp -> owner;
   }

**图 6：多个物理入口都可以参与发现和接管，设备只有一个控制 owner。** 这些入口并不是各有独立 VM 的设备副本。

主机/Core/协议关系沿用 :doc:`../architecture/index`。本图只展开 EV-MUX 之下的物理运行时链路；发现与 ROM loader 各有独立生命周期。串口/socket 适配器不再次接管会话。

.. list-table:: 物理接口比较
   :header-rows: 1
   :widths: 13 29 29 29

   * - sink
     - 物理形态
     - 连接/就绪判断
     - 常见用途与限制
   * - ``usj``
     - 芯片 USB-Serial/JTAG 外设，通常在主机呈现为串口。
     - 无可靠拔线检测时 ``present`` 为未知；静默断线依赖租约恢复。
     - P4-EYE 默认/回退口；还可能出现 ROM、IDF 原始启动输出。它与 USB-OTG CDC 不是同一个驱动。
   * - ``usb``
     - USB-OTG 使用现有 TinyUSB CDC；可与 MSC 在同一复合设备中出现。
     - USB 配置完成表示 present/ready；真实 USB 断开使 link generation 失效。
     - 适合交互、文件和图像；业务收发不要求 DTR。CDC 和 MSC 的功能与写入权限不同。
   * - ``uart``
     - 真实 UART TX/RX/GND 与 USB-UART 转接器。
     - 由板级 UART REPL 启用情况和实际初始化状态决定；通常不能可靠获知对端消失。
     - 可承载相同 channel，但受实际波特率、引脚/串口占用与有界发送期限限制。
   * - ``tcp``
     - 已配网设备的 STA IPv4 TCP server。
     - accept 分配槽位，FIN/RST、socket 错误和超时回收 link。
     - 默认 3232；两个 accepted slot，不等于两个 owner。浏览器需要 WebSocket-to-TCP 字节桥接。

板级后端启用条件
----------------

后端由 ``boards/<BOARD>/boardconfig.h``、``boards/<BOARD>/port/mpconfigboard.h`` 和 SoC 能力共同决定。关闭 MicroPython 接口宏会关闭对应接口，而非仅禁止 EV-MUX 使用它；当前没有 MUX 专用串口白名单。下表描述当前源码默认配置，运行时以 ``capabilities.sinks`` 为准。

.. list-table:: 编译条件与当前板级示例
   :header-rows: 1
   :widths: 15 35 50

   * - sink
     - 编译条件
     - 当前板级示例及运行限制
   * - ``usj``
     - SoC 提供 USJ 且 ``MICROPY_HW_ESP_USB_SERIAL_JTAG`` 启用。
     - P4 开发板及 ``ESP32_S31_CHATBOT``；部分 IDF system console 原始输出仍有独立配置。
   * - ``usb``
     - SoC/port 提供 USB device 且 ``MICROPY_HW_USB_CDC`` 有效。
     - ``ESP32_S3_EYE``、``ESP32_S31_KORVO``、``ESP32_S31_MOSAICO``、``AtomS3R-M12``；P4 板卡也可依据 SoC 支持同时暴露 CDC 与 USJ。
   * - ``uart``
     - ``MICROPY_HW_ENABLE_UART_REPL`` 启用，使用板级引脚与波特率。
     - P4、``ESP32_S3_EYE``、``AtomS3R-M12``、``ESP32_S31_KORVO`` 启用；当前 ``ESP32_S31_MOSAICO``、``ESP32_S31_CHATBOT`` 关闭。
   * - ``tcp``
     - ``ESP_VISION_ENABLE_TCP_TRANSPORT=1``，底层 WLAN 驱动可用。
     - STA 获得 IPv4 后才能监听。当前 ``ESP32_P4X_VISION`` 的 port 配置关闭 WLAN，不能由 TCP 编译开关推断其网络可运行。

板级例子见 ``boards/ESP32_P4X_EYE/boardconfig.h``、``boards/ESP32_S3_EYE/boardconfig.h`` 和 ``boards/ESP32_S31_MOSAICO/port/mpconfigboard.h``；后端约束由 ``platform/ev_transport_config.h`` 检查。

USB-Serial/JTAG 与 USB-OTG 怎么区分
-----------------------------------

P4-EYE 可以同时提供 USJ 和 OTG CDC，两路都可能呈现 ``ttyACM``，操作系统编号不稳定。主机应依据 USB 描述符、VID/PID、序列号和板卡连接拓扑识别接口，再用 ``hello``/``capabilities`` 确认设备身份。

USJ 与 TinyUSB CDC 使用不同外设/驱动路径。为其中一路发送 hello，响应只回到该路实际 link；不会因为另一接口是默认口就把响应复制过去。一个接口已持有会话时，另一个接口可发现设备，但其接管会返回忙。

P4-EYE 的运行态 CDC PID 当前为 ``0x4003``。部分 S3 构建使用 PID ``0x1001`` 以选择 esptool 的 USB reset 序列；PID 看起来像 Serial/JTAG，并不让 TinyUSB OTG CDC 获得 JTAG 硬件能力。ROM 下载模式可能重新枚举为另一个节点，不能继续假定原句柄有效。

link = 后端 + 槽位 + generation
-------------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     first [label = "TCP slot 0\ngeneration G"];
     close [label = "FIN / RST / close"];
     reuse [label = "TCP slot 0 reused\ngeneration G+1"];
     stale [label = "old queued request / frame\nslot 0, generation G"];
     check [label = "generation check\nreject stale work"];
     first -> close -> reuse;
     stale -> check; reuse -> check;
   }

**图 7：TCP 槽位复用不意味着旧连接继续存在。** 会话绑定准确的 link 引用；TX 目标、待执行 VM 请求和异步完成回调都需要避免向新连接投递旧业务。USB 真实断开与显式释放也会触发相应的失效边界。

板级配置不等于 IDF console 配置
-------------------------------

.. list-table:: 配置层
   :header-rows: 1
   :widths: 34 66

   * - 配置
     - 作用
   * - ``CONFIG_ESP_CONSOLE_*``
     - ESP-IDF system console、启动与异常路径的底层输出路由；不直接决定 EV-MUX owner。
   * - ``MICROPY_HW_ESP_USB_SERIAL_JTAG``
     - MicroPython/ESP-VISION USJ 编译与初始化。
   * - ``MICROPY_HW_USB_CDC`` / MSC 相关 port 宏
     - 现有 TinyUSB CDC/MSC 组成；不会安装第二套 USB driver 或 descriptor owner。
   * - ``MICROPY_HW_ENABLE_UART_REPL``
     - UART REPL 是否成为可注册的串口后端。
   * - ``ESP_VISION_TRANSPORT_DEFAULT_SINK`` / ``ESP_VISION_TRANSPORT_FALLBACK_SINK``
     - 无 owner 的默认/回退串口。必须是实际启用的 ``usj``、``usb`` 或 ``uart``；不能默认到尚未配网的 TCP。
   * - ``ESP_VISION_ENABLE_BACKGROUND_NETWORK``
     - 后台 Wi-Fi/TCP 服务策略；WLAN 硬件、原生/Hosted 驱动与 sdkconfig 仍需单独满足。
   * - ``ESP_VISION_WIFI_DEFAULT_SSID`` / ``ESP_VISION_WIFI_DEFAULT_PASSWORD``
     - 可选出厂 Wi-Fi 凭据。默认空；只在没有保存 NVS 记录时采用。
   * - ``ESP_VISION_ENABLE_TCP_TRANSPORT`` / ``ESP_VISION_TCP_PORT``
     - 板级 TCP 后端开关和监听端口，通常为 3232。

后端注册表在编译期确定，不提供运行期插件注册接口；USB 插拔、网络启停和 TCP 槽位状态仍是动态的。``supported``、``present``、``ready`` 和 ``owner`` 分别回答“有没有实现”“物理连接是否已知存在”“能否收发”“是否持有会话”，不能相互替代。

``console`` / ``null``（别名 ``none``）只是 ``ev_channel.c`` 的本地输出选项，不是公开的注册 sink，不出现在 ``capabilities.sinks``。不要用它们构造 v4 接管请求。

UART 为什么不能照搬 USB 的预览大小
----------------------------------

UART 的发送预算按整帧估算：

.. code-block:: text

   timeout_ms = max(1500, ceil(frameBytes * 10 * 1000 / baud) * 2 + 250)
   timeout_ms <= 10000

10 表示常见 8N1 每字节占用的位数。超过 10 秒预算的帧在提交前被拒绝；支持 ``preview.frame`` 并不保证任意 JPEG 都能在低波特率上传完。应提高波特率、降低图像分辨率/质量或改用 USB/TCP，仍须处理发送失败和短写。

TCP 的三个时间预算
------------------

1. **首次握手**：从未接管的连接约 3000 ms 后关闭，因此 TCP 客户端不应先做 300 ms 串口启动排空或长时间 UI 操作。
2. **会话租约**：默认 15000 ms，无合法授权活动则释放 owner；这一步是所有权回收。
3. **曾持有会话后的空闲回收**：失去 owner 后，socket 有约 30000 ms 窗口可重新接管；窗口到期后回收该准确 generation 的槽位。这一步是连接容量回收。

两个槽位允许 discovery/竞争连接短暂共存，不支持两个独立控制应用并行占用 VM。竞争接管失败的 TCP 连接在发送错误后关闭；正常释放后的同一 link 可以在规定窗口内重新接管。

DTR/RTS、RESET 与 ROM 下载
--------------------------

DTR 变化不会取得或释放 EV-MUX 会话。已关闭主机串口句柄但 USB 仍连接时，固件未必立即知道；正确做法是关闭前发送 ``session.release``，否则等租约到期。

CDC DTR/RTS 自动下载是另外一条 USB control 路径。它与 Python ``machine.bootloader()`` 共用 ``esp_vision_restart(download_mode)`` 底层函数；Python 请求可立即执行，USB control callback 只记录状态，延迟 timer 在回调外执行复位。P4 设置/清除下载位时保留系统寄存器其他位；S3 还遵循其 ROM USB persistence/PHY 交接契约。

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Firmware [label = "EV-MUX: machine.bootloader()\nor USB DTR/RTS control"];
     Firmware -> Restart [label = "esp_vision_restart(true)"];
     Restart -> ROM [label = "masked boot flag + esp_restart"];
     Host -> ROM [label = "reopen enumerated ROM interface"];
     Host -> ROM [label = "ROM/stub flash protocol + readback"];
     ROM -> Firmware [label = "clear download latch + reset"];
     Host -> Firmware [label = "new hello/acquire + identity check"];
   }

**图 8：进入下载模式、写镜像与恢复运行态是三个独立确认点。** EV-MUX 自身不传输 ROM 烧写命令；EV-MUX 端口消失也不能单独证明 ROM 已就绪或烧写已成功。

P4-EYE OTG 从运行态进入 ROM 时，esptool v5 使用 ``--before usb-reset``；已经进入 ROM、或等待重新枚举后重试时使用 ``--before no-reset``。v4 工具对应拼写为 ``usb_reset`` / ``no_reset``。下载工具还要确认写入、完整读回校验、下载位清除回读与运行态设备身份，不能把一个 ``EIO`` 直接当作成功。
