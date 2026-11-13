帧格式、逻辑通道与 RPC 协议
===========================

:link_to_translation:`en:[English]`

三个版本号
----------

``firmware.version`` 表示 Git/发布构建身份；``evMuxVersion`` 表示客户端必须遵守的会话协议；帧头 ``EVMUX/1`` 表示 wire framing。应先用 ``evMuxVersion`` 与 capabilities 判断兼容性，再以 firmware.version 展示、记录或校验指定固件，不按日期字符串猜测协议。

只读固件安装证据
----------------

``firmware.status`` 是 ``user.rpc`` 方法，仅启用 Iris 打包的 ``ESP32_S31_MOSAICO`` 构建声明 ``firmware.status.v1`` 能力。调用前先获取 EV-MUX v4 owner 会话。请求为 ``{"imageSize": N}``，N 必须是已校验包中应用 BIN 的准确字节数（1–7340032）。请求在传输任务执行，不进入 REPL、不执行 Python、不写入、不复位。缺失或错误长度返回 ``INVALID_ARGUMENT``，VM 未就绪返回 ``NOT_READY``，OTA 未确认或安装证据不可读返回 ``EVIDENCE_UNAVAILABLE``。

成功响应包含 ``ok=true``、``schemaVersion=1``、``deviceId``（出厂 MAC 的 12 位小写十六进制）、``board``、``firmware.id/version``、32 位十六进制 ``bootId``、``vmReady=true``、``bootConfirmed=true``、IDF 数值 ``resetReason`` 和 ``crashReset``。启动标识在 VM 软复位后保持不变，硬件重新启动后变化，安装确认适配将其前 8 字节按小端解释为 Iris Boot ID。

``application`` 包含 ``address``、分区 ``capacity``、请求的 ``imageSize``、完整 BIN 的 ``sha256``、``elfSha256`` 和 ``otaState="valid"``。完整 BIN 哈希包括指定长度内的全部字节和末尾镜像摘要，不等同于 ESP-IDF 的内嵌镜像哈希。``partitionTableSha256`` 覆盖配置分区表地址起的 4096 字节。``installation.operationId`` 和有符号 ``installation.result`` 只读返回保留安装器持久化的提交回执，不发送或实现其协议。回执缺失或格式错误时查询失败，不生成成功回执；非零结果原样返回，主机必须将其判为验收失败。

安装验收应对照本次目标，核对实时设备身份、板型、包内准确长度、哈希、布局和操作 ID，要求提交结果为零、``crashReset=false``，并在验收前后观察相同的启动标识。其他镜像或历史操作的健康响应不能证明本次安装成功。启动确认不验证用户 Python 代码或外设。本接口不进入 Vibe Mode，后续安装由用户通过 AI + Reset 手动进入。

一帧的字节布局
--------------

.. code-block:: text

   0x1e + ASCII "EVMUX/1 h=<H> p=<P> c=<CRC32>\r\n"
          + H bytes of UTF-8 metadata JSON
          + P bytes of payload
          + 0x1f

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     sof [label = "SOF\n0x1e"];
     hdr [label = "ASCII header\nh=H p=P c=CRC + CRLF"];
     meta [label = "H metadata bytes\nUTF-8 JSON object"];
     payload [label = "P payload bytes\ntext / JSON / JPEG"];
     eof [label = "EOF guard\n0x1f"];
     sof -> hdr -> meta -> payload -> eof;
   }

**图 12：只有 H/P 给出正文边界，EOF 字节不是 payload 分隔符。** JPEG、二进制文件数据或文本中可以包含 ``0x1e``、``0x1f``、CRLF；不能按这些字节截断正文。

H/P 使用 UTF-8 或实际二进制的**字节数**。例如字符“你”通常占三个 UTF-8 字节，不能把 JavaScript 字符串 length 直接当成 payload_bytes。

CRC32 只覆盖 payload，不覆盖 metadata/header；使用标准 CRC32（主机 Python 可用 ``zlib.crc32(payload) & 0xffffffff``），以八位十六进制表示。设备生成的帧带 payload CRC；入站命令 ``c=00000000`` 被解释为跳过校验。CRC 提供损坏检测，不提供身份认证，也不证明操作最终完成。

JPEG 响应和预览还需独立验证图像有效性。固件在入队前校验编码输出，Core 在交付前再次检查 JPEG 结构与尺寸；不能只凭 EV-MUX 帧完整或 CRC 正确就交付图像。坏抓图响应在本地返回 ``INVALID_IMAGE``，坏预览整帧丢弃；校验范围及限制见 :doc:`preview-debug`。

主机必须处理任意拆包与粘包
--------------------------

.. code-block:: text

   read #1: [SOF][header first half]
   read #2: [header tail][metadata first half]
   read #3: [metadata tail][payload first half]
   read #4: [payload tail][EOF][whole next frame][next SOF]

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     sof [label = "seek SOF"];
     header [label = "read bounded header"];
     meta [label = "read exactly H bytes"];
     payload [label = "read exactly P bytes"];
     eof [label = "check EOF + payload CRC"];
     dispatch [label = "validate metadata / scope\ndispatch complete frame"];
     sof -> header -> meta -> payload -> eof -> dispatch;
     dispatch -> sof;
   }

**图 13：流式 parser 保存跨 read 的状态，完整帧验证后才交付。** 若格式、长度、CRC 或期限不满足，应记诊断并执行受控重同步；不得把半帧送进 REPL 或文件写入。

当前设备 RX metadata 上限 384 字节、payload 上限 2048 字节。RX parser 有约 3000 ms 空闲和 10000 ms 整帧期限。这些是**设备接收主机命令**的限制，不是 preview 最大图像尺寸，也不应机械作为主机收大图的超时。

metadata 字段
-------------

.. list-table:: 公共字段
   :header-rows: 1
   :widths: 23 30 47

   * - 字段
     - 类型
     - 用途
   * - ``channel``
     - 字符串
     - 指定 channel，例如 ``user.rpc``、``repl.stdout``。
   * - ``type``
     - 字符串
     - RPC 请求 ``req``、响应 ``rsp``、异步通知 ``event``、数据 ``data``。
   * - ``seq``
     - uint32
     - 请求响应相关序号；不同 channel/类型上的相同值不能互相完成请求。
   * - ``method``
     - 字符串
     - RPC 方法或 stdio 来源描述；不能把 ``c-stdio`` 当成 RPC 调用。
   * - ``encoding``
     - 可选字符串
     - ``json``、``text``、``binary`` 等语义提示。
   * - ``sessionId``
     - 32 字符小写十六进制字符串
     - 当前 owner 授权/输出范围；发现请求不带 token。
   * - ``vmEpoch``
     - uint32
     - VM 相关操作及输出范围。
   * - ``sid``
     - 可选字符串
     - 逻辑来源/域，例如 ``stdio``；不是 sessionId。
   * - ``ts_ms``
     - 可选整数
     - 设备时间戳，不与主机时钟天然同步。
   * - ``contentType``、``width``、``height``
     - 按 channel 定义
     - 描述图像 payload；JPEG 字节仍在 payload，而非 JSON 中。

metadata 与 RPC 请求 payload 必须是完整顶层 JSON object；已知字段类型正确、不能重复。未知扩展字段可以忽略。字符串中的转义不能用简单的逗号分割器解析，JSON 后多余垃圾也不能被视为完整合法对象。

channel 与 stream 对照
----------------------

.. list-table:: 线上通道
   :header-rows: 1
   :widths: 22 13 15 50

   * - channel
     - 方向
     - stream
     - payload / 用途
   * - ``user.rpc``
     - 双向
     - user
     - JSON 控制：发现、会话、脚本、设备/网络事务。
   * - ``debug.rpc``
     - 双向
     - debug
     - 调试 RPC；capture 的成功响应直接是二进制 JPEG。
   * - ``repl.stdin``
     - 主机→设备
     - user
     - 原始 REPL 输入字节；有 session/epoch 授权与独立长度上限。
   * - ``repl.signal``
     - 主机→设备
     - user
     - 当前仅合法单字节 ``0x03``，表示 Ctrl-C。
   * - ``repl.stdout``
     - 设备→主机
     - user
     - Python/REPL/HAL 与 C stdout 的文本块。
   * - ``repl.stderr``
     - 设备→主机
     - user
     - C stderr 等显式 stderr 路径；Python 异常也可能在 stdout。
   * - ``log.idf``
     - 设备→主机
     - debug
     - IDF/平台日志文本。
   * - ``preview.frame``
     - 设备→主机
     - user
     - ``img.flush()`` 产生的 JPEG 数据帧。

所有 channel 所属 stream 固定；host 不为 preview、stdout 和 RPC 各绑定一个不同串口。owner 持有两组 stream，RPC 响应回到请求的准确 link。

RPC 方法清单与执行层
--------------------

.. list-table:: EV-ATP 控制面
   :header-rows: 1
   :widths: 22 30 18 30

   * - channel
     - method
     - 执行层
     - 说明
   * - user.rpc
     - ``hello`` / ``capabilities``
     - transport
     - 发现；hello 可显式接管。
   * - user.rpc
     - ``session.keepalive`` / ``session.release``
     - transport
     - 续租、查询 ready/epoch、释放 owner。
   * - user.rpc
     - ``transport.uart.prepare`` / ``transport.uart.probe`` / ``transport.uart.commit``
     - transport
     - UART owner 协商波特率；提交或回退前暂停普通 UART 输出。
   * - user.rpc
     - ``file.write``
     - transport / native VFS
     - 可靠二进制上传；绑定会话/VM 代次，不执行解释器。
   * - user.rpc
     - ``script.write`` / ``script.run``
     - VM
     - 旧版 Base64 写入 / 排入脚本执行。
   * - user.rpc
     - ``device.control``
     - 主要为 transport
     - stop/reset/status、network.*、storage.writer.*、launcher 状态等按 action 分发。
   * - debug.rpc
     - ``transport.state`` / ``route.get``
     - transport
     - 状态与路由观测，不更改公开 stream 绑定。
   * - debug.rpc
     - ``debug.info``
     - VM，stats 例外
     - device/memory/sensor/fs.list/fs.read；transport.stats 不依赖 VM。
   * - debug.rpc
     - ``debug.capture_frame``
     - VM
     - 在此 RPC rsp 中编码应用最近发布的独立采帧副本；不初始化传感器或物理采帧。要求 ``debug.capture_frame.observed.v1`` 能力。
   * - debug.rpc
     - ``debug.capture_screen``
     - VM
     - LCD 最后 RGB565 帧编码 JPEG，在此 RPC rsp 中返回；不调用摄像头。
   * - debug.rpc
     - ``route.bind`` / ``route.auto``
     - 拒绝
     - v4 返回 ``UNSUPPORTED_OPERATION``。

高层 Device Core 的 ``files.read/remove`` 与 ``python.execute`` 是客户端 API，不代表线上存在同名 RPC。文件与 Python 的具体组合见 :doc:`python-files`。

``debug.capture_screen`` 请求通过 ``debug.rpc`` 携带当前会话与 VM epoch，可选 ``quality`` 为 1–100。成功响应保持原请求 seq/method，``encoding:binary``、``contentType:image/jpeg``，携带 ``width`` / ``height`` 与 JPEG payload。无 LCD 返回 JSON error ``NOT_SUPPORTED``，尚无有效显示帧返回 ``DISPLAY_NOT_READY``；不能用摄像头图像代替。Core 的 ``capture({ source: 'screen', format: 'jpeg' })`` 使用这一 RPC，详见 :doc:`preview-debug`。

请求、响应与异步事件可以交错
----------------------------

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Device [label = "RPC seq=20: debug.info"];
     Host <-- Device [label = "data: repl.stdout"];
     Host <-- Device [label = "data: preview.frame"];
     Host <-- Device [label = "event: network.changed"];
     Host <-- Device [label = "rsp: debug.info, seq=20"];
   }

**图 14：等待 seq=20 的响应时，reader 仍应处理其他完整帧。** 响应匹配至少比较 type、seq、channel、method；随后按适用规则检查 session/epoch。``event/hello`` 即使恰好使用同 seq，也不能作为 ``rsp/hello``。

接管响应可分配新的 session；发现响应没有 owner 授权范围。VM 成功响应必须属于请求的 epoch；控制响应可能报告更晚的当前 epoch。部分授权失败响应不带 session 字段，须按错误码处理，不采纳里面的值为新的授权。

收到 owner 的 ``repl.stdout``、``repl.stderr``、``preview.frame`` 时，拒绝其他 session 或旧 VM 的输出；图像与控制回复不能根据“最近的请求”猜归属。

一个可验证的主机编码器
----------------------

以下为**主机 CPython**示例，用于解释 framing；真正通信推荐使用 :doc:`host-integration` 中的共享客户端。

.. code-block:: python

   import json
   import zlib

   def frame(metadata, payload):
       meta = json.dumps(metadata, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
       data = payload if isinstance(payload, bytes) else payload.encode("utf-8")
       crc = zlib.crc32(data) & 0xffffffff
       header = ("\x1eEVMUX/1 h=%d p=%d c=%08x\r\n" %
                 (len(meta), len(data), crc)).encode("ascii")
       return header + meta + data + b"\x1f"

   request = frame(
       {"channel": "user.rpc", "type": "req", "method": "hello",
        "seq": 1, "encoding": "json"},
       json.dumps({"evMuxVersion": 4, "acquire": True,
                   "clientNonce": "0123456789abcdef0123456789abcdef"}),
   )

该函数只编码帧，不实现会话、并发 reader、期限、RPC 关联或恢复。它不能替代完整客户端，也不能在设备 MicroPython 中直接假定可用主机 ``zlib``/线程/socket 组合。

file.write 是经过会话授权后的有界例外：二进制 payload 可携带 16384 字节文件内容和 20 字节 ID/offset 前缀，并校验 CRC32；其他控制 payload 仍限制 2048 字节。begin/data/commit/abort 见 :doc:`python-files`。

运行态 UART 波特率协商
--------------------------------

启用 UART 的固件通过 ``capabilities.uartBaud`` 发布 ``baseline:115200``、``current``、``candidates:[2000000,1500000,921600,460800,115200]``、``probeBytes:512``、``timeoutMs:5000`` 和 ``switchDelayMs:100``。未启用 UART 时该字段为 ``null``。这些参数用于应用运行期间的通信；ROM/stub 烧录使用独立的协议与连接生命周期。

已授权的 UART owner 在 ``user.rpc`` 上携带当前 session ID 调用以下传输层方法，VM 软复位期间仍可调用。USB-OTG、USB-Serial-JTAG 和 TCP owner 无权改变 UART 波特率。

1. ``transport.uart.prepare`` 接受 ``{baudRate,token}``，速率须为公布的候选值，token 为 1–32 个 ASCII 字母、数字、下划线或连字符。固件以 **旧速率** 返回 ``{ok:true,baudRate,token,timeoutMs,switchDelayMs}``。协商期间暂停普通 UART 输出，让已开始的帧发送完毕，并确认硬件 FIFO **及移位寄存器** 排空，再在公布的延迟后切换。主机传输适配器重新打开或调整串口，并等待公布的延迟加保护时间后开始探测。
2. ``transport.uart.probe`` 接受 ``{token,data}``，data 为 1–512 字节可打印 ASCII。响应原样返回 ``token``、``data`` 以及 ``baudRate``。主机使用不同长度和内容进行多组双向校验，按候选列表顺序选择首个通过的速率并停止。探测帧使用普通 EV-MUX CRC32。
3. 至少一次探测后调用 ``transport.uart.commit``，参数为 ``{token}``，响应为 ``{ok:true,token,baudRate,...}``。提交响应进入硬件后恢复普通输出。相同已提交 token 的 probe/commit 支持幂等重试，以恢复丢失的最终确认。

未完成的协商在 5000 ms 后超时，释放 owner、恢复 115200，并清空 UART RX 与解析状态。协商成功后，正常释放会话或租约过期同样恢复 115200；丢失最终提交响应的情况可能需要通过 15000 ms 会话租约恢复。硬复位从 115200 启动，VM 软复位保留物理速率与会话生命周期。旧固件未发布该能力时保持初始速率，主机不得试发变速命令。
