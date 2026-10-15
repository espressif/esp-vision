预览、调试抓帧、背压与统计
==========================

:link_to_translation:`en:[English]`

preview 是数据流，不是订阅 RPC
------------------------------

应用调用 ``img.flush()`` 后，preview producer 在需要发送时将当前图像编码为 JPEG，以 ``preview.frame`` 数据帧发给当前 user stream owner。没有额外的 ``preview.subscribe`` 方法，也不通过 ``print`` 输出 Base64 图像。

.. code-block:: python

   import sensor
   import time

   sensor.reset()
   try:
       sensor.set_pixformat(sensor.RGB565)
       sensor.set_framesize(sensor.QQVGA)
       while True:
           img = sensor.snapshot()
           img.flush()
           time.sleep_ms(200)
   finally:
       sensor.shutdown()

无限循环由主机显式中断；``finally`` 提供应用资源释放路径。这个设备端示例不自动保证帧一定到达或保持某个帧率，主机仍需验证完整 JPEG 与 session/epoch。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     image [label = "sensor.snapshot()\nimage.Image"];
     flush [label = "img.flush()"];
     gate [label = "owner / handshake / vmReady\npreview queue availability"];
     jpg [label = "JPEG encode"];
     newest [label = "latest pending preview\nreplace stale pending frame"];
     mux [label = "preview.frame\nbinary payload + CRC"];
     host [label = "verify scope + decode JPEG\nrender latest frame"];
     image -> flush -> gate -> jpg -> newest -> mux -> host;
   }

**图 18：先判断发送资格和队列条件，再编码、提交图像。** 无有效 owner 时不为主机编码 preview；相机是否继续采集是用户程序自己的行为。

图像帧长什么样
--------------

以下为概念性 metadata；实际 seq、route、时间戳、sessionId、vmEpoch 均由当前发送快照产生：

.. code-block:: json

   {
     "sid": "preview", "seq": 40, "channel": "preview.frame",
     "type": "data", "encoding": "binary", "route": "usb",
     "contentType": "image/jpeg", "width": 160, "height": 120,
     "ts_ms": 123456,
     "sessionId": "89abcdef0123456789abcdef01234567", "vmEpoch": 1
   }

payload 直接从 JPEG 的二进制字节开始，p 是编码后 JPEG 字节数，不是 width × height，也不是 source RGB565 帧缓冲大小。主机不应把 JPEG payload 交给 JSON/UTF-8 parser。

``seq`` 可帮助观察帧序列，但丢帧不应按“缺哪一帧就补传哪一帧”理解。主机可保留最新完整图像用于渲染，旧 session/epoch 的图像必须丢弃，不能在软复位后闪回上一程序的画面。

Preview、摄像头与 LCD 三条图像路径
----------------------------------

.. list-table:: 明确区分图像来源
   :header-rows: 1
   :widths: 25 25 25 25

   * - 属性
     - ``preview.frame``
     - ``debug.capture_frame``
     - ``debug.capture_screen``
   * - 触发
     - 应用 ``img.flush()``。
     - 主机摄像头抓帧请求。
     - 主机 LCD 抓屏请求。
   * - 来源
     - 应用提交的 image。
     - 摄像头 snapshot。
     - 显示层最后提交的 RGB565 帧。
   * - 线上类型
     - 独立 ``type:data`` JPEG。
     - 原 RPC ``type:rsp`` 携带二进制 JPEG。
     - 原 RPC ``type:rsp`` 携带二进制 JPEG。
   * - 执行
     - 应用持有图像并提交 preview。
     - VM 队列初始化/使用摄像头。
     - VM 队列读取显示缓冲并编码，不调用摄像头。
   * - 交付
     - 允许丢帧，最新待发帧优先。
     - 按请求 seq/channel/method、会话与 epoch 关联响应。
     - 相同 RPC 关联规则，无图像时返回错误。

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Device [label = "debug.rpc req: capture_screen, seq=50"];
     Device -> VM [label = "last display RGB565 frame / JPEG encode"];
     Host <-- Device [label = "debug.rpc rsp, seq=50\nencoding=binary, image/jpeg"];
     Application -> Device [label = "img.flush()"];
     Host <-- Device [label = "preview.frame data\nnot capture RPC response"];
   }

**图 19：单次抓图由原 RPC 响应交付，不等待另一个图像 channel。** Core 对外统一为 ``capture({ source: 'camera' | 'screen', format: 'jpeg' })``；两种来源不会相互回退。成功结果是 ``{ data, mimeType, metadata }``，metadata 包含尺寸；失败仍可为 JSON error，客户端须先检查 encoding/contentType。

LCD 抓屏不初始化屏幕、不重绘、不启动摄像头，只读取最后有效 RGB565 显示帧。没有 LCD 返回 ``NOT_SUPPORTED``，未初始化或没有有效显示帧返回 ``DISPLAY_NOT_READY``。读取及编码在 VM 任务中与显示操作、软复位清理串行执行，VM 忙时也可能返回 ``VM_BUSY`` / ``VM_TIMEOUT``。可选 JPEG ``quality`` 为 1–100。它反映软件显示帧而非物理面板回读，不能证明背光或屏幕硬件正常。

使用统一 Core 真机 CLI 的 ``--screen-output`` 显示测试图案并保存 JPEG；具体命令与验收要求见 :doc:`validation`。

队列优先级与丢帧
----------------

控制、会话、REPL、日志、preview 对应不同 TX 类。普通小帧队列预算有界，控制流量有预留预算；不能用不读取主机数据的方式验证“所有输出永不丢失”。

preview 只保留最新待发帧，可替换尚未开始发送的旧图。已经发送到一半的字节不能撤回，也不能把两张 JPEG 的尾巴拼成一张。preview 与大图 debug response 共用有限特殊缓冲：最多三个按需分配的 256 KiB 缓冲，不代表永远可容纳三个任意大图或允许 256 KiB 的主机命令。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     f1 [label = "pending F1"];
     f2 [label = "new F2 replaces F1"];
     f3 [label = "new F3 replaces F2"];
     send [label = "TX sends latest F3"];
     read [label = "host continuously reads"];
     f1 -> f2 -> f3 -> send -> read;
   }

**图 20：降低积压时延比交付每一张预览更重要。** 若需要保存全部图像，应使用设备端存储或专门的视频/记录接口，并独立设计其容量、回放与验收；preview 不承担这个保证。

UART 的帧期限随 baud 与总字节数计算；USB/TCP 也有有界等待。底层 write 可只接受一部分字节或返回 WOULD_BLOCK，TX task 必须保留未发送后缀并在期限内继续。帧发送失败或 socket 错误可能使 TCP link 关闭，而 RX 半帧超时只重置仍在线连接的 parser，不应一概认为设备重启。

日志通道与系统日志 UI
---------------------

IDE 可以将 ``log.idf``、RPC 诊断和连接事件合并显示为“系统日志”，但这个 UI 面板不是 firmware 的第九个 channel。``repl.stdout`` 则主要显示应用/REPL 文本，channel 的真实来源与限制见 :doc:`architecture`。

系统日志的格式、颜色、过滤器可以由主机选择，不能反过来改变固件所有权和物理路由。无 owner 的默认/回退日志、启动 console 原始文本与当前 session 业务输出，应在诊断记录中分开标识。

如何看待统计计数
----------------

``debug.info`` 的 ``scope:transport.stats`` 与 ``transport.state`` 可在用户 Python 忙时查询，用于观察队列、RX 拒绝、发送超时、preview 丢弃和后端状态。必须区分 endpoint/link 统计、全局统计、业务完成标记：

* 未读的 USJ 默认输出积压可增加全局 drop；这不等于活动 TCP 上的特定 RPC 已失败。
* 某个 RPC 已拿到完整关联响应，不代表整段应用业务完成。
* preview 丢弃是允许的实时行为，不自动否定文件传输，但错误 owner/epoch 的图像被接受属于协议错误。
* reset/generation 变化可能改变统计范围；应把采样时刻、端点和会话身份与结果一起记录。

诊断时保持有界，只记录必要的计数与状态；不要在错误路径中再制造大量打印以填满同一 TX 队列。

帧缓冲的生命周期
-----------------

LCD 缓存在显示初始化时分配，反初始化时释放。SPI 彩色数据传输完成后绘图函数才返回，后续绘图不会覆盖仍被 DMA 借用的数据。摄像头 snapshot 缓存在首次抓帧时分配，由 Python 与 Core 摄像头抓图共用；相同尺寸的请求复用同一个指针。实际像素格式/尺寸变化、摄像头关闭/重置或 VM 软重置会释放它。两者都不会在开机时预占整帧内存。分配优先使用 PSRAM，不可用时保留原有通用堆回退；仅配置启用不能证明 PSRAM 初始化成功。

地址只在本次分配的生命周期内稳定，不保证重启或重新配置后相同。Python snapshot 图像借用该缓存，若要跨下一次抓帧保留图像，应先复制；Core 在主机端返回独立接收的 JPEG。摄像头驱动 DMA 缓冲和 JPEG 编码工作缓冲有各自的生命周期，这两块缓存不是图像功能的全部内存预算。
