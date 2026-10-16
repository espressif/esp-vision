架构、执行上下文与标准输出
==========================

:link_to_translation:`en:[English]`

系统架构与职责边界
----------------------------------------

IDE、Agent、CLI 和自定义工具是平级主机应用，共用同一个 Device Core 包。Core 实现主机 API 与协议/会话状态；EV-MUX 是 USB CDC、USJ、UART、TCP 上的运行时线协议；固件实现设备操作和 MicroPython 运行时。下图中的代表性 API 仅作总览，不是完整清单。

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: 主机应用、Device Core、EV-MUX 与设备固件

   统一系统架构图；下文展开固件执行路径。

本 SVG 与 README、系统架构及 SDK 指南共用。发现和 ROM 烧写独立于 EV-MUX 运行时路径，具体边界见 :doc:`../architecture/index`；设备脚本在开发板执行，不要求主机持续连接。

EV-MUX 在哪一层
---------------

EV-MUX 是运行期字节封装和多路复用层。它不执行 Python，也不把原有 MicroPython VM 替换成“EV-MUX VM”。MicroPython 保留词法分析、编译、解释执行、友好 REPL、stdin/stdout 和异常打印；ESP-VISION 在 HAL 和平台服务边界接入帧协议与会话路由。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     vm [label = "MicroPython VM\ncompiler / REPL / user code"];
     hal [label = "mp_hal_stdin_rx_chr\nmp_hal_stdout_tx_strn"];
     stdio [label = "ev_stdio\nframing hooks"];
     ctrl [label = "ev_control_transport\nparser / auth / RPC"];
     q [label = "REPL byte queue\nVM request queue"];
     session [label = "ev_session / ev_channel\nowner / epoch / routing"];
     tx [label = "ev_mux\nTX scheduling / frame CRC"];
     backend [label = "ev_transport\nUSB / UART / TCP"];
     vm -> hal -> stdio -> session -> tx -> backend;
     backend -> ctrl -> q -> vm;
     ctrl -> session;
   }

**图 2：解释器层、HAL 适配层、控制面、会话路由与物理后端的关系。** 上行 stdout 和下行 stdin 没有绕开 VM；纯传输控制 RPC 则可以不等待用户 Python 返回。

输出来源决定通道，而不是文本内容
--------------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     python [label = "print / sys.stdout\nREPL prompt / traceback"];
     pystderr [label = "MicroPython sys.stderr\nshared stdio implementation"];
     hal [label = "mp_hal_stdout_tx_strn"];
     cout [label = "C printf / stdout"];
     cerr [label = "C fprintf(stderr, ...)" ];
     log [label = "ESP_LOGx"];
     hout [label = "newlib stdout hook"];
     herr [label = "newlib stderr hook"];
     hlog [label = "esp_log_set_vprintf hook"];
     out [label = "repl.stdout\nUSER stream"];
     err [label = "repl.stderr\nUSER stream"];
     logs [label = "log.idf\nDEBUG stream"];
     python -> hal -> out; pystderr -> hal;
     cout -> hout -> out; cerr -> herr -> err; log -> hlog -> logs;
   }

**图 3：当前构建中各类打印的真实去向。** Python 的 ``print()`` 与 C 的 ``printf()`` 可能进入相同 channel，但调用链不同；日志文字中包含 “error” 并不会把它自动变成 ``repl.stderr``。

.. list-table:: 输出路径核对表
   :header-rows: 1
   :widths: 24 42 34

   * - 来源
     - 固件入口
     - 线上结果
   * - Python ``print()``、``sys.stdout.write()``、友好 REPL 提示符
     - MicroPython print/stdio → ``mp_hal_stdout_tx_strn()`` → ``ev_stdio_write_repl_stdout()``
     - ``repl.stdout``，``encoding:text``。
   * - REPL 中的未处理异常、``KeyboardInterrupt`` traceback
     - ``pyexec.c`` 使用 ``mp_obj_print_exception(&mp_plat_print, ...)``，仍进入 HAL stdout。
     - 当前可能出现在 ``repl.stdout``，不能仅观察 stderr 判定失败。
   * - 当前 MicroPython ``sys.stderr.write()``
     - ``shared/runtime/sys_stdio_mphal.c`` 的 stdout/stderr 对象共享 ``stdio_write()``；两者调用 cooked HAL stdout。
     - 当前仍进入 ``repl.stdout``，并非桌面 CPython 的独立 stderr 管道。
   * - C ``printf()`` / ``fprintf(stdout, ...)``
     - ``CONFIG_LIBC_NEWLIB`` 构建安装 newlib ``stdout->_write`` hook。
     - ``repl.stdout``，metadata 的 ``method:c-stdio`` 表示 C stdio 来源。
   * - C ``fprintf(stderr, ...)``
     - newlib ``stderr->_write`` hook。
     - ``repl.stderr``，``method:c-stdio``。
   * - ESP-IDF ``ESP_LOGI/W/E`` 等正常日志
     - ``esp_log_set_vprintf()`` → ``ev_stdio_log_vprintf()``。
     - ``log.idf``，``method:esp-idf``；走 debug stream。
   * - ``esp_vision_debug_printf()`` 等平台日志
     - 平台日志路径调用 EV-MUX IDF log 写入函数。
     - ``log.idf``；具体 ``method`` 取决于调用入口。
   * - ``img.flush()``
     - preview producer → JPEG 编码 → lossy EV-MUX TX。
     - ``preview.frame``，二进制 JPEG；不经过 printf。

``encoding:text`` 不保证每个帧都在一个 UTF-8 字符或一行结束处切分。主机需要保留增量 UTF-8 解码器和未完成行缓冲；不能逐帧独立解码并把替换字符当作原始文本。

.. note::

   上表描述当前源码，不能由通道名字反推所有语言层的 stderr 行为。如果将来为 Python stderr 增加专门 HAL 路径，应同时更新本表、客户端判断和协议测试。

IDF system console 与 MicroPython console
-----------------------------------------

IDF system console 的 Kconfig 选择底层系统输出目的地。MicroPython 的 port 配置决定其 UART REPL、USJ、TinyUSB CDC 是否编译和初始化。EV-MUX 的会话路由再决定运行期业务帧发往哪一个已启用 link。这是三个配置层，不是一组互为别名的开关。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     boot [label = "ROM / bootloader\nearly IDF logs"];
     idf [label = "IDF system console\nCONFIG_ESP_CONSOLE_*"];
     raw [label = "Raw UART / USJ bytes\nno EV-MUX contract"];
     mp [label = "MicroPython port config\nUART / USJ / TinyUSB"];
     hooks [label = "ev_stdio_init0\ninstall hooks + enable mux"];
     run [label = "Runtime EV-MUX frames\nactive owner link"];
     boot -> idf -> raw;
     mp -> hooks -> run;
   }

**图 4：初始化前的 system console 与运行态 EV-MUX 输出不是同一契约。** 设备上电、进入 ROM 或发生绕过 hook 的 panic 时，主机可能读到没有帧头的文字；活动业务仍必须靠完整帧确认。

``MICROPY_HW_ESP_USB_SERIAL_JTAG`` 决定 ESP-VISION/MicroPython 的 USJ 路径。即使它为 0，某些 IDF target/version 的默认 system console 仍可能使用 USJ。反过来，启用 MicroPython USJ 时，端口初始化会显式打开外设时钟和 internal PHY，不依赖 console 顺带完成初始化。

``sensor.evmux(False)`` 是运行期调试开关，可以恢复非 EV-MUX 的文本路径。当前源码的 ``ESP_VISION_EV_MUX_DEFAULT_ENABLED`` 默认值在 ``ev_stdio.c`` 内定义，但该文件没有接入 ``boardconfig.h`` 的这项设置；只在板级文件定义同名宏不会关闭上电默认启用。主机不应把关闭 framing 当作 v4 的降级协商机制。

哪个任务负责哪类工作
--------------------

.. list-table:: 执行上下文
   :header-rows: 1
   :widths: 25 42 33

   * - 上下文
     - 主要工作
     - 与用户 Python 的关系
   * - transport task
     - 读取各 link、拼帧、检查授权，处理发现、保活、释放、网络事务和纯 C transport stats。
     - 用户脚本执行时仍能处理这些控制，不取得另一套 MicroPython VM。
   * - VM task
     - 友好 REPL、Python 用户代码、触及 VFS/对象/相机的 VM RPC。
     - 长脚本可能占用该任务；队列有界，需先停止脚本再做依赖 VM 空闲的操作。
   * - TX task
     - 发送完整帧，处理短写、拥塞、队列优先级与 link 失效。
     - 生产者提交的数据被复制或转移到受控生命周期，不能直接悬挂 VM 临时指针。
   * - network worker / TCP 管理
     - Wi-Fi 初始化、事务性配网、连接/监听维护、TCP 槽位回收。
     - 与 Python WLAN 共享底层服务；不会因普通 VM 软复位自动重复初始化无线驱动。
   * - USB CDC 回调 / restart timer
     - 回调记录 DTR/RTS 状态，延迟处理下载/复位。
     - 不在 USB control callback 内立即拆除驱动；与 EV-MUX 接管是不同控制面。

``file.write`` 在 transport task 中直接写原生 VFS，检查会话/VM 代次并持有存储交接计数，不进入解释器队列。旧版 ``script.write``、``script.run``、``debug.capture_frame``、``debug.capture_screen`` 和涉及 Python 状态的 ``debug.info`` 进入 VM 请求队列。``debug.info`` 的 ``scope:transport.stats`` 是例外：它在 VM 忙时也可查询。队列数组有两个槽位，以环形队列空槽规则实际只容纳一个待执行请求；过期预算为 5000 ms，忙或过期会报告 ``VM_BUSY`` / ``VM_TIMEOUT``。

LCD 抓屏在同一 VM 任务中读取显示层最后提交的 RGB565 帧并编码 JPEG，不调用 ``sensor`` 或摄像头，不改变显示内容。它不是 preview 订阅，也不是物理面板回读；与 display/软复位清理串行执行，避免读取已释放的帧缓冲。

``repl.signal`` 中的 Ctrl-C 在解释器已启用中断时调度 ``KeyboardInterrupt``，否则作为编辑控制字符进入 REPL 输入队列。它不是远程抢占 FreeRTOS 任务，也不能保证及时中断不返回 VM 的原生函数。

软复位与后台通信的生命周期
--------------------------

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Transport [label = "soft-reset request / REPL command"];
     Transport -> VM [label = "stop input admission; advance epoch"];
     VM -> VM [label = "touch / display\ncamera / VFS\nGC cleanup"];
     VM -> VM [label = "new VM\nboot.py\nmain.py"];
     VM -> Transport [label = "vmReady=true; event/hello"];
     Host -> Transport [label = "session.keepalive"];
     Host <-- Transport [label = "same owner; newer epoch; vmReady"];
     Host -> VM [label = "current-epoch REPL barrier"];
     Host <-- VM [label = "new complete marker"];
   }

**图 5：软复位推进 VM 代次，但可以保留会话、网络和可保留的 link。** 先停止旧 VM 输入，再清理 LCD touch、display、preview、camera 等对象。``vmEpoch`` 变化发生在复位开始，不是“VM 已可执行”的证据；须继续等待 ``vmReady`` 与真实 REPL 标记。

队列、日志失败与不会发生的回退
------------------------------

正常队列每端点 16 KiB / 8 帧，两个 TCP slot 共享一个队列预算；另为控制流量预留 4 KiB / 2 帧。短 REPL 写入有 512 字节 staging 与约 5 ms 合并窗口，避免每个字符生成一个完整帧。preview 只保留最新待发图像，详见 :doc:`preview-debug`。

EV-MUX 已启用时，帧提交/发送失败不会把同一条 Python 或 C 输出再次广播到原始串口。stdio hook 有递归保护；在拥塞错误路径中再次打印日志可能造成递归，固件改用计数器观测丢弃。当前 IDF vprintf 临时缓冲为 512 字节，格式化结果放不下时记录 drop 并返回错误，不把超长日志伪装为完整成功输出。

初始化前与关闭 mux 后的既有 console 路径、低层 panic/ROM 输出是另外的生命周期边界，不应与“发送失败后回退”混为一谈。

阅读源码的入口
--------------

``platform/ev_stdio.c`` 与 ``overlay/micropython/ports/esp32/mphalport.c`` 决定输入输出接入；``platform/ev_control_transport.c`` 决定入站控制分流；``platform/ev_session.c`` / ``ev_channel.c`` 决定所有权与路由；``platform/ev_mux.c`` 决定输出封装、快照、队列和发送；``platform/ev_vm_output.c`` 管理 VM 输出范围；``platform/main.c`` 管理启动和复位。MicroPython stderr 的关键上游文件是 ``lib/micropython/shared/runtime/sys_stdio_mphal.c``。
