帧格式、逻辑通道与 RPC 协议
===========================

:link_to_translation:`en:[English]`

三个版本号
----------

``firmware.version`` 表示 Git/发布构建身份；``evMuxVersion`` 表示客户端必须遵守的会话协议；帧头 ``EVMUX/1`` 表示 wire framing。应先用 ``evMuxVersion`` 与 capabilities 判断兼容性，再以 firmware.version 展示、记录或校验指定固件，不按日期字符串猜测协议。

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
     - 抓一张图，在此 RPC 的 rsp 中返回 JPEG。
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
