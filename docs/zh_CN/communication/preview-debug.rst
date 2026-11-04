预览、调试抓帧、背压与统计
==========================

:link_to_translation:`en:[English]`

preview 图像由应用主动发布
---------------------------

应用调用 ``img.flush()`` 后，preview producer 在需要发送时将当前图像编码为 JPEG，以 ``preview.frame`` 数据帧发给当前 user stream owner。只读 TCP 旁观者可独立订阅，详见下文。 已绑定的 USB 摄像头输出也是独立消费者，详见 :doc:`../api-reference/uvc`；它不接管 EV-MUX 控制会话，也不初始化相机。

UVC 使用 USB 等时传输。任何失败或短包都会终止当前帧，固件不会把后续 JPEG 片段继续拼到不完整的前缀上。它会尝试发送一次仅含 UVC 头的 ``ERR|EOF`` 结束包，将失败与完整帧传输分开报告，并释放源缓冲槽。下一帧切换 FID 并清除错误标志。总线复位或 alternate setting 取消也会释放旧源。这不保证 USB 无损交付；验收必须完整解码所有保留的 UVC 帧，并覆盖 CDC/TCP 并发场景，不能只检查输出文件非空。

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

**图 18：先判断发送资格和队列条件，再编码、提交图像。** 没有有效 owner、只读旁观者或已绑定 UVC 消费者时，不编码 preview；相机是否继续采集是用户程序自己的行为。

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
     - 应用最近一次采帧后发布的摄像头图像。
     - 显示层最后提交的 RGB565 帧。
   * - 线上类型
     - 独立 ``type:data`` JPEG。
     - 原 RPC ``type:rsp`` 携带二进制 JPEG。
     - 原 RPC ``type:rsp`` 携带二进制 JPEG。
   * - 执行
     - 应用持有图像并提交 preview。
     - VM 队列读取独立且只读的采帧副本。
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

发送前，共享 JPEG 编码器先确认返回长度为正且不超过已分配缓冲区，并验证图像结构及其与源图像的尺寸一致性。摄像头抓图、LCD 抓屏和预览均经过这一入口。异常输出不会作为图像进入队列；抓图返回 ``JPEG_FAILED``。Core 在 EV-MUX 长度、EOF、CRC 和 session/epoch 检查之后验证 JPEG 结构及元数据尺寸。坏响应仅使对应抓图立即返回 ``INVALID_IMAGE``；坏预览记录诊断后整帧丢弃，不进入图像回调。

图像检查覆盖自包含的 8 位 Huffman JPEG 头、有界标记段与编码表引用、扫描边界及无尾随数据的最终 EOI，不对熵编码像素执行完整解码。完整解码验证仍属于真机验收。链路中途断开仍可能留下部分字节；接收端必须等待完整、校验通过的 EV-MUX 帧，丢弃不完整输入，不能直接显示。

摄像头抓图是被动观察，要求固件声明 ``debug.capture_frame.observed.v1`` 能力。它编码应用最近一次成功采帧后发布的独立副本，包括 ``sensor.snapshot(buffer)`` 使用调用方缓存的路径，以及 ``sensor.skip_frames()`` 采到的帧。它不会初始化传感器、额外物理采帧、写入应用持有的 ``Image``，也不会在采帧返回后继续借用调用方缓存。应用后续的图像绘制应通过 ``img.flush()`` 预览或 LCD 缓存观察，这个来源保持采帧时的原始像素。Device Core 0.6 会拒绝向缺少此能力的旧固件发送不安全的摄像头 RPC；只升级 Core 并不会让旧固件具备安全抓图行为。

没有有效发布帧时返回 ``CAMERA_FRAME_NOT_READY``：包括应用首次采帧前、采帧失败后、摄像头重配/关闭/重置或 VM 软重置后。请求不会启动空闲摄像头。输出尺寸、像素格式、镜像或翻转实际变化时，在改变硬件之前使旧帧失效；重配失败也可能已经改变硬件，不能继续返回旧帧。同值设置保留当前帧。IPA 模式实际变化也会在控制器停流/重启之前使旧帧失效，重启失败也不保留旧帧；同值模式保留当前帧。手动 ISP 写入在访问硬件前保守地使旧帧失效，包括部分失败，即使请求值相同也如此。这些变化同样会释放观察副本的分配失败状态。应用下一次成功采帧会发布带有新尺寸/格式的图像。摄像头观察与 LCD 抓图不会相互替代。

摄像头发布、状态修改与 JPEG 编码均在主 VM 任务执行。Python 工作任务修改摄像头时返回 ``OSError``，不会与采帧、编码或释放并发。transport task 只排队请求，不持有摄像头像素。排队请求可因忙或超时返回 ``VM_BUSY`` / ``VM_TIMEOUT``；取消、所有者丢失或 epoch 变化可丢弃响应，但不会采帧或改写应用缓存。VM 软重置先停止接收请求，再释放两份摄像头缓存；硬件重置后两份缓存均为空。

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

LCD 缓存在显示初始化时分配，反初始化时释放。SPI 彩色传输完成后绘图函数才返回，后续绘图不会覆盖仍被 DMA 借用的缓存。应用默认 snapshot 缓存在首次使用时分配，后续应用采帧可复用同一缓存；显式传入的缓存仍归调用方所有。应用若要跨自己的下一次采帧或摄像头重配保留图像，仍需自行复制。

安全的摄像头观察持有独立原始帧副本，在应用首次成功采帧后按需分配，尺寸稳定时复用。例如 QVGA RGB565 会在应用 snapshot 缓存之外增加 153,600 字节；开机时不预占缓存，每次主机请求也不会新分配一帧。副本优先使用 PSRAM，失败时回退到支持 8 位访问的内部堆；配置启用不能证明 PSRAM 初始化成功。各板后端先完成 DMA/PPA 与缓存同步，再复制 CPU 可读的结果；观察副本不会直接交给摄像头 DMA。

观察副本分配失败时，应用采帧仍成功、像素保持不变，之后摄像头观察持续返回 ``NO_MEM``，直到释放此分配失败状态；即使后续某次采帧失败，也不会每帧反复尝试分配。摄像头重配/关闭/重置或 VM 软重置会释放这个状态，使应用后续采帧可重新尝试分配。实际重配在修改硬件前释放观察副本，VM 清理也释放默认应用 snapshot 缓存。固件借用的观察视图只在本次 VM 调用内有效；Core 在主机端返回独立接收的 JPEG。驱动 DMA、PPA、JPEG 和传输缓存有各自生命周期，均需计入图像功能的总内存预算。

TCP 只读预览
--------------

预览客户端连接同一个 TCP 端口（默认 3232），在 3000 ms 握手期限内发送 ``user.rpc/preview.subscribe``，payload 为 ``{}``，无需先接管控制会话。成功响应为 ``{"ok":true,"subscribed":true,"channel":"preview.frame","maxObservers":2}``。发送 ``preview.unsubscribe`` 和 ``{}`` 可取消订阅。两者均幂等，不携带 ``sessionId`` 或 ``vmEpoch``。capabilities 发布 ``preview.observers.v1``、``preview.subscribe`` 和 ``preview.unsubscribe``。

最多两个旁观者可与独占的 USB、USJ、UART 或 TCP 控制者并存。第三个旁观者收到 ``OBSERVER_LIMIT``，三个 TCP 槽中保留一个给控制连接。旁观者连接的 ``hello`` 返回 ``READ_ONLY``；Python、文件、REPL 和设备控制仍须通过 owner 授权。需要控制时使用另一条连接。旁观者只收到主动发布的图像和自身请求的响应，预览 metadata 不含控制者的会话 ID 或执行身份；其中 ``route`` 保留当前 user stream 的路由值，旁观者 socket 独立传输。这种角色隔离不增加网络身份认证。

应用通过 ``img.flush()`` 发布处理后的图像，连接本身不会启动 sensor。复用控制预览和已绑定 UVC 的 JPEG 准备过程，不增加 HTTP 服务或重复 JPEG 编码。每次发布只为所有旁观者复制一份不可变 JPEG、计算一次 CRC，每个连接独立维护发送位置。TCP 通过 scatter/gather ``sendmsg`` 聚合帧头、metadata、JPEG 和帧尾，避免每个片段单独 send。TCP 仍须向每个旁观者发送数据，因此网络带宽随人数增加；既有 owner 预览池保持独立。

RX 解析器按实际端点槽位静态分配，避免扩大 TCP 容量时重复为串口分配闲置解析器。TCP 发送时临时内存不足按背压处理，仍受同一有界期限约束。socket 关闭操作在共享 I/O 锁外进行，并使用短发送超时限制 lwIP 分配 FIN 时的重试等待。``debug.info`` 的 ``scope:transport.stats`` 提供 ``lastTxTimeout``，记录最近一次超时的端点、槽位、发送位置、总字节数、经过时间和无进展时间。

旁观者共用最多三个延迟分配、持续复用的 256 KiB PSRAM 槽（上限 768 KiB），每个旁观者有两个小型帧描述符。尚未开始发送的帧可替换为最新帧；只读帧总期限为 8000 ms，无进展期限为 4000 ms，分别通过 ``tcp.observerFrameTimeoutMs`` 和 ``tcp.observerIdleTimeoutMs`` 上报。每次发送进展刷新空闲计时，替换首字节尚未发出的帧不会刷新任一期限。已开始的帧必须在这些预算内完成，否则关闭该 socket。慢旁观者不会占住控制者或另一旁观者的队列。link generation 校验防止旧帧及旧订阅串入重连后的 socket。订阅期间无需控制租约或 keepalive，空闲时可保持连接；取消订阅后恢复普通空闲回收。

主机脚本仅使用 Python 标准库和内置有界 EV-MUX 解析器；``--view`` 还需要 PATH 上存在 ``ffplay``，其显示队列仅保留最新待显示图像。传输解析始终保留帧边界并校验 JPEG CRC。

.. code-block:: console

   python tools/preview_tcp.py 192.168.1.100 --view
   python tools/preview_tcp.py 192.168.1.100 --duration 10 --save-last preview.jpg

可显式运行 ``tools/test_preview_tcp.py --host <设备IP> --port <串口> --device-cli <已安装插件>/cli/device-cli.mjs --output-dir <项目内目录>`` 验收；加入 ``--video <V4L2节点>`` 可检查 UVC 并发。测试会替换当前 Python 应用，运行有限时长测试程序，使用 Pillow 解码收到的 JPEG，结束后恢复 UVC logo。
