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
     bindings [label = "绑定层 (modules/)\nsensor / image / display / espdl / tflite"];
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

- **绑定层**\ （\ ``modules/``\ ）：即 ``USER_C_MODULES`` 层。主要模块 ``image``、 ``sensor``、``display``、``espdl`` 和 ``tflite`` 通过 ``MP_REGISTER_MODULE`` 自注册。 ``py_imageio.c`` 提供 ``image.ImageIO`` 类型，``py_helper.c`` 为共享辅助代码。 绑定层只做对象转换与轻量 API 适配，重逻辑放在纯 C 或 ``platform/`` 中。
- **平台服务**\ （\ ``platform/``\ ）：自研的 ESP32 胶水层。``ev_channel.c`` / ``ev_mux.c`` / ``ev_control_transport.c`` / ``ev_stdio.c``\ （EV-MUX / EV-ATP 底层传输）、``preview.c``\ （EV-MUX JPEG 预览）、``display.c``\ （通用显示层）、``sdcard.c``\ （挂载到 ``/sdcard``\ ）、 ``usb_msc.c``\ （通过 TinyUSB MSC 暴露 ``ffat`` 分区）、``jpeg.c``\ （硬件或软件 JPEG）、 ``debug.c``\ ，以及 ``main.c``\ （启动初始化与软复位循环）。
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
     proc   [label = "imlib 处理 /\nAI 推理"];
     lcd    [label = "display.write()\n-> LCD"];
     prev   [label = "img.flush()\n-> EV-MUX 预览"];

     sensor -> snap -> img;
     img -> proc;
     img -> lcd;
     img -> prev;
   }

``sensor.snapshot()`` 将一帧采集到可复用的帧缓冲中，并封装为 ``image.Image``。脚本随后 对图像进行 ``imlib`` 处理、ESP-DL 推理或 TFLite Micro 推理，再通过 ``display.write()`` 送往 LCD，或通过 ``img.flush()`` 送往主机预览。

底层传输机制
------------

ESP-VISION 的主机传输分为三层：物理 sink、路由 stream、EV-MUX 逻辑 channel。

物理 sink
~~~~~~~~~

``platform/ev_channel.c`` 定义可写入的物理目标：

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - sink
     - 含义
   * - ``usj``
     - USB-Serial-JTAG。在提供该外设的板卡上可用；没有 host 占用 USB-OTG CDC 时优先作为回退 sink。
   * - ``cdc``
     - USB-OTG CDC。在可用时，host 打开端口（DTR）即成为所有 stream 的自动路由。同时提供 MSC 文件预览。
   * - ``uart``
     - 用于运行时重定向（``route.bind`` / ``route.auto``）的有线辅助 sink；永远不会被自动选中。stream 绑定到 UART 后，也从 UART 接收其 framed 输入。
   * - ``console``
     - MicroPython stdout fallback。
   * - ``null``
     - 丢弃输出；``none`` 是它的别名。

``cdc`` 上报两种状态：``present`` 表示 USB-OTG 接口已枚举；``ready`` 表示有 host 占用端口（DTR 有效）。只有 ``ready`` 决定路由——设计中不存在激活 RPC、租约或心跳。

路由 stream
~~~~~~~~~~~

``ev_stream_t`` 是物理路由面的抽象，数量少于 EV-MUX channel。路由操作的单位是完整 stream，而不是单个逻辑 channel：一个 stream 绑定到某个 sink 后，归属于该 stream 的所有 channel 都使用该 sink。Device -> Host 的帧写入该 sink；Host -> Device 的对应 channel 帧只接受从该 sink 进入。

所有 stream 的自动路由由同一条规则决定：**host 占用 USB-OTG 端口（DTR）期间活跃 sink 为 ``cdc``，否则使用板级首选回退（有 USJ 时为 USJ，再回退到 console）。** 专用 transport 任务监视 DTR 边沿并把它应用到全部两个 stream；每次切换都按 stream 发出 ``route.changed``\ （reason 为 ``cdc_connected`` / ``cdc_disconnected``）。host 断开或崩溃会释放 DTR，因此回退是自动的——无需任何保活。

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - stream
     - 承载内容
     - 路由策略
   * - ``EV_STREAM_USER``
     - 正常工作面：``user.rpc``、``repl.stdin``、``repl.signal``、``repl.stdout``、``repl.stderr``、``preview.frame``
     - 跟随活跃 sink（上述 DTR 规则）。
   * - ``EV_STREAM_DEBUG``
     - 调试与系统流量：``debug.rpc`` / ``log.idf``
     - 跟随活跃 sink，因此已连接的 USB 端口承载所有逻辑 channel。

``route.bind`` 把单个 stream 固定到某个 sink（运行时重定向，例如把 ``debug`` 重定向到 ``uart`` 做有线日志采集）；手动绑定的 stream 脱离 DTR 规则，直到 ``route.auto`` 恢复自动路由。

EV-MUX channel
~~~~~~~~~~~~~~

EV-MUX channel 是端到端协议语义面，不等同于物理端口。每个 channel 固定归属于一个 stream，host 不得为单个 channel 单独选择 sink。``direction`` 描述帧方向；Device -> Host 按所属 stream 的当前 sink 输出，Host -> Device 则按所属 stream 的当前 sink 做入站授权。

.. list-table::
   :header-rows: 1
   :widths: 22 20 22 24 34

   * - channel
     - direction
     - 所属 stream
     - payload 类型
     - 用途
   * - ``user.rpc``
     - 双向
     - ``user``\ （``rsp`` / ``event``）
     - JSON
     - 正常工作控制，例如 ``hello``、``capabilities``、``script.write``、``script.run``、``device.control``。
   * - ``debug.rpc``
     - 双向
     - ``debug``\ （``rsp`` / ``event``）
     - JSON；``debug.capture_frame`` 的应答帧自身直接携带二进制 JPEG payload
     - 系统/调试 EV-ATP 请求、响应、事件、错误；``req`` 为 Host -> Device，``rsp`` / ``event`` 为 Device -> Host。
   * - ``repl.stdin``
     - Host -> Device
     - ``user``
     - text
     - Host 输入到 REPL，不是输出 stream。
   * - ``repl.signal``
     - Host -> Device
     - ``user``
     - small binary/text
     - Host 发送 Ctrl-C 等信号。
   * - ``repl.stdout``
     - Device -> Host
     - ``user``
     - text
     - Python ``print()``、REPL prompt、C stdout。
   * - ``repl.stderr``
     - Device -> Host
     - ``user``
     - text
     - Python exception、C stderr。
   * - ``log.idf``
     - Device -> Host
     - ``debug``
     - text
     - ESP-IDF ``ESP_LOGx`` 输出。
   * - ``preview.frame``
     - Device -> Host
     - ``user``
     - binary JPEG
     - ``img.flush()`` 持续产生的预览帧；无需订阅、不经 RPC。

EV-MUX 帧格式
~~~~~~~~~~~~~

所有 EV-MUX 帧都使用长度前缀，payload 可以是二进制：

.. code-block:: text

   \x1eEVMUX/1 h=<metadata_len> p=<payload_len> c=<crc32>\r\n
   <metadata JSON bytes>
   <payload bytes>
   \x1f

``h`` 与 ``p`` 是权威长度；``0x1f`` 只作为帧尾保护与重同步辅助。固件生成的帧带有 payload CRC32。Host -> Device 的命令帧可以使用 ``c=00000000``，固件接收端把 0 视为“跳过 CRC 校验”。

EV-ATP 控制
~~~~~~~~~~~

EV-ATP RPC 按语义拆分：

- ``user.rpc`` 承载 IDE 正常工作控制：``hello``、``capabilities``、``script.write``、``script.run``、``device.control``。
- ``debug.rpc`` 承载系统/调试命令：``transport.state``、``route.get``、``route.bind``、``route.auto``、``debug.info``、``debug.capture_frame``。应答回到请求到达的 sink。``debug.capture_frame`` 的图片由 ``debug.rpc`` 应答帧自身携带（metadata 带 ``contentType=image/jpeg`` 与 ``width`` / ``height``），不存在独立的数据 channel。

``route.changed`` 是 ``user.rpc`` / ``user`` 上的路由事件，不是 ``debug.rpc`` 事件。它报告 stream 级别的 sink 变化（reason 为 ``cdc_connected`` / ``cdc_disconnected``），host 收到后必须更新该 stream 下所有 channel 的连接状态。

路由本身不需要任何 RPC：打开 USB-OTG 端口即成为活跃 sink，关闭则选择板级回退。设计中刻意不提供激活、租约或心跳方法——线路状态（DTR）就是完整的路由协议。

Host 对接契约
~~~~~~~~~~~~~

上位机同一时刻只使用一条物理 USB 连接；在双 USB 板卡上，USB-OTG 与 USJ 是等价单选项，任一端口都承载全部 channel。连接顺序如下：

#. 打开所选端口。EV-MUX 上电默认启用，无需任何 REPL 访问：USB 链路出现时设备会发出 bootstrap ``hello`` 事件（每次软复位后也会重发）。
#. 发送 framed ``hello`` 请求（或消费启动事件），再请求 ``capabilities``；确认 ``firmware.id`` 为 ``esp-vision``，检查 ``evMuxVersion``，并协商固件声明的 feature/channel。
#. 正常使用设备：REPL channel、``script.write`` / ``script.run``、预览、``debug.rpc``、``log.idf`` 全部在已打开的端口上可用。不存在激活步骤，也不存在心跳；端口即路由。
#. 断开时直接关闭端口即可。USB-OTG 端口关闭（或线缆脱落）后，所有 stream 自动选择板级回退，并发出 ``route.changed`` 事件上报。

``sensor.evmux()`` 保留为调试开关（例如用 ``sensor.evmux(False)`` 恢复纯文本 REPL）；板级如需关闭上电默认启用，可在 ``boardconfig.h`` 把 ``ESP_VISION_EV_MUX_DEFAULT_ENABLED`` 定义为 ``0``。

``hello``、``capabilities`` 和 ``debug.info`` 的 device payload 都包含相同的 ``firmware`` 对象。其中稳定的 ``id`` 为 ``esp-vision``；``version`` 使用 ESP-IDF 根据 Git 自动生成的 ``PROJECT_VER``：正式发布构建返回 ``YYYY.MM.DD`` tag，未打 tag 的开发构建带 ``-N-g<commit>`` 后缀（工作区构建还可能带 ``-dirty``）。Host 可以用这个构建身份做版本展示和升级提示，但协议兼容性必须根据 ``evMuxVersion`` 与声明的 capabilities 判断，不能只比较发布版本字符串。

Host 的 demux 必须逐帧读取 ``metadata.channel``，再按 channel 分发，不能依据端口或先前帧推断当前帧类型。推荐维护静态 ``channel -> stream`` 表和动态 ``stream -> sink`` 表；路由变化只更新后者。

接收分流原则
~~~~~~~~~~~~

固件接收侧只保留字节流拼帧所需的状态。每个完整 EV-MUX 帧到达后，先根据实际入站 sink 校验其所属 stream，再立即按 ``metadata.channel`` / ``type`` / ``method`` 分流：

- ``repl.stdin`` 写入 REPL 输入环形缓冲。
- ``repl.signal`` 调度键盘中断等信号。
- ``user.rpc`` / ``debug.rpc`` 先选择 RPC domain，再按 ``method`` 执行 EV-ATP 控制逻辑。

入站授权是单一白名单：发现方法 ``hello`` / ``capabilities`` 在任一 USB sink 上都受理；其余 channel 只从所属 stream 的当前路由受理。显式 ``route.bind`` 到 UART 后，也可从 UART 接收 framed 控制。业务层不得依赖"当前模式"推断帧类型。

传输执行模型
~~~~~~~~~~~~

EV-MUX 上电默认启用（``ESP_VISION_EV_MUX_DEFAULT_ENABLED``），控制面无需 REPL 访问即可探测；每次软复位后 mux 状态与路由都回到同一组确定初值。

帧接收不依赖 REPL。专用 transport 任务（``ev_transport``，见 ``platform/ev_control_transport.c``）拥有接收路径：泵送 TinyUSB 设备栈、把 USJ/CDC/UART 三个入站环形缓冲喂给各 ingress 的帧 parser、分发完整帧，并应用 DTR 路由规则（活跃 sink 切换与 ``route.changed`` 发出）。``mp_hal_stdin_rx_chr()`` 不再解析帧；EV-MUX 开启时它只消费 framed ``repl.stdin`` 字节。

分发按执行上下文拆分：

- transport 任务 RPC（即使用户代码运行中也立即应答）：``hello``、``capabilities``、``transport.state``、``route.*``、``device.control``。``repl.signal`` 由 transport 任务调度键盘中断，host Ctrl-C 可以到达正在运行的脚本。
- VM 任务 RPC（排队到解释器，在 ``mp_hal_stdin_rx_chr()`` 上下文执行）：``script.write``、``script.run``、``debug.info``、``debug.capture_frame``——所有涉及 MicroPython 对象、VFS 或 camera 的方法。队列满时回复 ``VM_BUSY`` 错误。
- ``repl.stdin`` payload 写入 framed REPL 输入环形缓冲，由 REPL 循环消费。

EV-MUX 开启后，所有物理字节流只承载帧：USJ/CDC/UART 底层接收路径不再把 ``0x03`` 解释为 Ctrl-C（中断语义只属于 framed ``repl.signal`` channel），不成帧的字节由帧 parser 的 SOF 重同步丢弃。

已知传输问题
~~~~~~~~~~~~

以下 USB 传输问题已知，并与正常 EV-MUX / EV-ATP 路由契约分开跟踪：

- USB MSC 当前通过 TinyUSB callback 直接暴露 ``ffat`` / ``vfs`` 分区。MSC 写入尚未与 MicroPython VFS 或 IDE 文件写入协调，因此 host MSC 访问与脚本/文件操作并发时可能破坏文件系统。在写入协调实现前，MSC 应按只读能力处理，或与 IDE 文件写入互斥。

实现说明（保留观测手段）：

- 帧写入使用带进度检测与明确超时的 bounded write-all loop（``platform/ev_mux.c``）；底层 sink 上报真实写字节数，partial write 会确定性地中止该帧并计入统计，而不是静默截断。
- 预览帧是唯一的有损类别。写停滞的 sink 会被标记为拥塞并进入短暂冷却；拥塞期间 ``preview.frame`` 帧在写入前整帧丢弃（``ev_mux_write_lossy``），因此线路上的字节流始终可解析。由于 USB-Serial-JTAG 吞吐远低于 OTG CDC，usj 路由下生产侧还把预览限速为每 100ms 一帧。RPC 与 REPL 帧永不被该机制丢弃。
- 三个 ingress 输入环形缓冲均为 2048 字节。``debug.info`` 的 ``transport.stats`` scope（纯 C，VM 忙时仍可应答）上报每 ingress 的 RX 字节数与 ring 满事件、完整/畸形/拒绝帧计数，以及 replRx/VM_BUSY/TX 写超时/TX 拥塞丢弃（``txDrop``）计数。replRx 溢出目前只计数，尚无独立的 host overflow 事件。

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
