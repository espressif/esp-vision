通信系统与 EV-MUX
=================

:link_to_translation:`en:[English]`

本章介绍 ESP-VISION 的整个主机通信系统：从 MicroPython 的输入输出、ESP-IDF 日志和摄像头数据，到物理接口、会话所有权、EV-MUX 帧，再经共享 Device Core SDK 到达 IDE、Agent、CLI 或自定义主机应用。协议说明以当前仓库的 EV-MUX v4 实现为准；固件发布版本、EV-MUX 会话版本和线上帧格式版本是三个不同概念。

.. important::

   ``evMuxVersion:4`` 是会话与授权契约；``EVMUX/1`` 是字节帧格式。v4 固件继续使用 EVMUX/1，不应把帧头改成 EVMUX/4。IDE 1.3.0 的 Device Core 只接受明确支持 v4 的固件。

一张图看懂整条链路
------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     default_node_color = "#edf4ff";
     default_fontsize = 12;
     py [label = "MicroPython\nprint / REPL / files"];
     c [label = "C stdio / ESP_LOGx"];
     img [label = "img.flush()\nJPEG preview"];
     hook [label = "HAL / stdio hooks\npreview producer"];
     channel [label = "EV-MUX channels\nuser + debug streams"];
     session [label = "One session owner\nlink + epoch checks"];
     mux [label = "EVMUX/1 frames\nbounded TX queues"];
     io [label = "usj / usb / uart / tcp"];
     transport [label = "Host byte transport\nserial / TCP / WebSocket bridge"];
     core [label = "Device Core\nprotocol / sessions / operations"];
     view [label = "IDE / Agent / CLI / custom tools\nUI / workflows / commands"];
     py -> hook; c -> hook; img -> hook;
     hook -> channel -> session -> mux -> io -> transport -> core -> view;
   }

**图 1：设备输出经过语义分流、会话约束和帧封装，再进入同一条物理连接。** 主机命令沿相反方向到达 parser、授权检查和 REPL/控制处理器。主机必须持续收包，因为请求响应、文本输出和图像可以交错出现。

推荐阅读顺序
------------

.. toctree::
   :maxdepth: 1

   architecture
   interfaces
   sessions
   wire-protocol
   python-files
   preview-debug
   host-integration
   network-discovery
   device-core
   validation

.. list-table:: 按问题定位
   :header-rows: 1
   :widths: 40 60

   * - 想解决的问题
     - 阅读入口
   * - Python print、printf、IDF log 为什么不是同一条输出？
     - :doc:`architecture`
   * - 两个 USB 口、UART、Wi-Fi/TCP 分别做什么？
     - :doc:`interfaces`
   * - 两台主机争用、断线、软复位如何恢复？
     - :doc:`sessions`
   * - 怎样实现自己的帧 parser 和 RPC 客户端？
     - :doc:`wire-protocol`
   * - 怎样输入 Python、读写文件和运行长脚本？
     - :doc:`python-files`
   * - preview 和 debug.capture_frame 有什么不同？
     - :doc:`preview-debug`
   * - 怎样配网、使用共享客户端并处理启动残包？
     - :doc:`host-integration`
   * - 如何从 npm 安装 Device Core，并接入串口/TCP、文件与预览？
     - :doc:`device-core`
   * - 怎样证明命令真的完成，怎样定位问题？
     - :doc:`validation`

六个必须分开的概念
------------------

* **物理接口 / sink**：后端类型，例如 ``usj`` 或 ``tcp``。
* **link**：一次实际连接，以后端、槽位和 generation 标识；同一个 TCP 槽位再次连接仍是新 link。
* **stream**：内部路由分组，只有 ``user``、``debug`` 两组，不是八个独立串口。
* **channel**：帧的业务语义，例如 ``repl.stdout``、``preview.frame``。
* **sessionId**：某个 link 持有设备所有权的凭证；不能迁移到另一个 socket。
* **vmEpoch**：MicroPython VM 的代次。会话仍存在，不表示旧 VM 的工作仍有效。

当前实现边界
------------

整个设备同时只有一个会话 owner，并一起拥有 user/debug 两个 stream。其他连接可以发现设备，但不能并行执行另一个控制会话。所谓“抢占”在本实现中是争用接管资格与显式交接，不存在强制踢走有效 owner 的接口。

TCP 是基于 STA IPv4 的原始字节流服务，默认端口 3232，最多两个已接受的 TCP link；它不是 WebSocket、HTTP、MicroPython WebREPL，也不提供本版之外的身份认证或传输加密。浏览器通过主机侧字节桥接访问 TCP，详见 :doc:`host-integration`。

ROM 下载模式与 EV-MUX 运行态属于不同协议阶段；MicroPython 的 REPL 仍是解释器控制台，而 IDF system console 仍承担初始化前、ROM/bootloader 及部分致命故障输出。不能把这些边界上的原始文字当成合法业务帧。
