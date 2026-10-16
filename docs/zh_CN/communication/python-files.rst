交互式 Python、脚本与文件操作
=============================

:link_to_translation:`en:[English]`

一个解释器，多种主机操作
------------------------

友好 REPL、脚本执行和文件查询共用一个 MicroPython VM；二进制上传在相同会话、VM 代次和存储交接门控下直接写原生 VFS，不执行 Python。Core 仍串行化文件与解释器操作。一个长时间占用 VM 的程序，会影响 REPL 和需要 VM 的 RPC，但不会让 transport task 自动失去保活/网络控制能力。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     ui [label = "Host user action\nterminal / run / file"];
     core [label = "Device Core\npython.input() / files.write()\npython.execute() / control()"];
     text [label = "repl.stdin\nPython bytes"];
     rpc [label = "user.rpc\nscript.run"];
     binary [label = "user.rpc\nfile.write (binary)"];
     native [label = "native VFS\ncontinuous write + SHA-256"];
     stop [label = "repl.signal\n0x03"];
     queue [label = "auth + bounded VM/REPL queues"];
     vm [label = "same MicroPython VM\nREPL / VFS / execfile"];
     output [label = "repl.stdout / stderr\nmarkers + application output"];
     ui -> core;
     core -> text; core -> rpc; core -> stop; core -> binary;
     binary -> native;
     text -> queue; rpc -> queue; stop -> queue;
     queue -> vm -> output;
   }

**图 15：交互输入、文件命令和运行命令是不同入口，但不提供独立解释器隔离。** 一个 session 的 owner 应协调这些操作，不能让两个调用方同时拼接 REPL 命令。

交互式 Python：framed stdin
---------------------------

以下 metadata 与 payload 组成一帧，示例 token/epoch 仅为占位，实际必须来自本次接管：

.. code-block:: json

   {"seq":10,"channel":"repl.stdin","type":"data","encoding":"text","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

.. code-block:: python

   print("hello from current VM")

Python 文本之后还应发送适合 REPL 的行结束，例如 ``\r\n``。``repl.stdin`` payload 是 Python 字节，不是 ``{"code":"..."}`` JSON。该 channel 本身不是 RPC，不为每次输入返回同 seq 的 ``rsp``；主机观察 REPL 输出并自行关联执行标记。

当前 capabilities 的 ``replInputMax`` 通常为 511 字节，来自 512 字节环形队列的有效容量。必须按实际编码后长度限制每个输入帧；多行输入、中文、emoji、Python 转义、CRLF 都占用字节。不要把 2048 字节控制 payload 限额误用于 stdin。

Device Core 使用 ASCII 转义的 Python literal 构造执行行，并将多行源码放进 ``exec(...)``，避免中文源码的 UTF-8 字节预算与解释器输入容量不匹配。**具体编码策略是客户端实现，不是 firmware 自动转义任意超长源码的能力。**

Ctrl-C、Ctrl-D 与停止程序
-------------------------

Ctrl-C 必须经 ``repl.signal``，payload 只允许单字节 ``0x03``：

.. code-block:: json

   {"seq":11,"channel":"repl.signal","type":"signal","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

.. code-block:: text

   payload hex: 03

EV-MUX 启用时，在物理串口上裸发 ``0x03`` 不会被当作 Ctrl-C，它是无效/不完整 framing 输入。将字符 “03” 或 JSON ``3`` 填入 payload 也不是同一个字节。

友好 REPL 的 Ctrl-D（``0x04``）可以作为 stdin 控制字节触发其软复位行为；Device Core 的软复位流程也可通过 framed Python 命令请求。``device.control`` 的 ``action:stop`` 提供控制面的停止入口，但停止请求被接受不等于被中断代码已退出。

停止后，应等待当前 session/epoch 中新的完整标记，证明 REPL 可执行。脚本若正在无法及时返回的原生调用中，Ctrl-C 可能延后；不能由一次 RPC ack 推断它已释放相机/文件资源。

完成标记与命令回显
------------------

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> VM [label = "framed Python command + unique marker"];
     Host <-- VM [label = "echo: print('MARKER', ...) source"];
     Host <-- VM [label = "application stdout / log / preview"];
     Host <-- VM [label = "actual complete MARKER line"];
     Host -> Host [label = "match current scope + full line + values"];
   }

**图 16：回显包含标记字符串，实际执行后的打印行才是完成依据。** 主机需要跨帧拼接完整行；仅用字符串 contains 查找 marker，会把回显误认为成功。

标记应每次运行唯一。结合 sessionId/vmEpoch、完整行结构和预期结果值判断；对摄像头任务，还要验证实际图像尺寸或其他应用证据。异常可能出现在 stdout，stderr 为空也不是成功依据。

可靠二进制文件上传
----------------------------------------

SDK 0.2.0 的 Python、模型、图片与其他字节文件共用一个上传引擎；大脚本执行先经该引擎上传临时源码，再 script.run。公开 files.write 参数保持不变。旧 v4 固件缺少 file.write.binary 时仍可查询，但须更新固件后才能上传；Core 不静默回退旧写入器。固件的 script.write 只为此前已发布客户端保留。

控制和数据均使用 user.rpc 的 file.write 方法，携带当前 sessionId/vmEpoch 与对应 seq。小型 JSON 控制仍限制 2048 字节。主机在 begin 前生成新的 16 字节随机 upload ID，begin 响应丢失或取消时仍可针对该 ID 发 abort。

.. code-block:: json

   {"action":"begin","path":"/models/example.tflite","mode":"overwrite","totalBytes":2097152,"sha256":"<64 lowercase hex>","uploadId":"<32 lowercase hex>"}
   {"ok":true,"uploadId":"<same ID>","chunkBytes":16384}

数据请求 metadata 设置 encoding:binary，payload 为 16 字节原始 upload ID、4 字节小端 offset，随后是 1..16384 字节文件原始内容；CRC32 覆盖整个 payload（含前缀）。非最后一块按 4096 字节对齐，最后一块可较短。NUL、CRLF、SOF、EOF 都是普通数据，不做 Base64 或 Python 输入解析。固件返回 JSON {ok:true,writtenBytes:N}，Core 校验后才发送下一块。重复、跳跃和乱序 offset 被拒绝，不提供随机写、自动重放或断点续传。

.. code-block:: json

   {"action":"commit","uploadId":"<same ID>"}
   {"ok":true,"bytes":2097152,"complete":true}
   {"action":"abort","uploadId":"<same ID>"}

固件在整个上传期间保持一个原生临时文件打开，并持有一次完整 VFS 操作的准入计数，阻止 MSC 在活跃描述符尚未关闭时交接。commit 校验总长度与流式 SHA-256，随后 fsync、close、发布；空文件只需 begin/commit。接收器只有在确认当前所有者的 file.write 二进制请求后才分配大块帧存储，其他控制 RPC 限额不变；二进制 payload 上限为 16384+20 字节，与 JSON 的 2048 字节限额分开。

上传绑定物理接口代次、会话、VM epoch 和会话转换代次；旧所有者不能追加或取消新的上传。abort、旧代次、失去会话或 30 秒无文件进展会关闭并删除临时文件；关闭失败保留存储交接屏障，不伪造释放成功。虽然原生 I/O 不执行解释器，VM 复位仍使上传失效。启动时不预留文件传输大缓冲。

传输与完整性检查期间保留原目标。FAT 无法单次重命名覆盖已有目标，因此替换时先保留旧文件备份，已校验临时文件发布成功后才删除备份，普通发布失败会尝试回滚。FAT 目录更新之间掉电可能留下临时/备份文件，不承诺日志式掉电原子替换。commit 成功但响应丢失时，主机无法确认完成状态，重试前应核验目标。SHA-256 校验内容一致性，不用于认证恶意主机。

上传设计与实测性能
---------------------------------------

文件本质是字节序列，扩展名不改变传输语义；上传与执行是不同操作，因此 Python、模型和图片共用一个二进制引擎。整次传输只打开、哈希和关闭一次，替代每小块重新 open/append/close。原始 16 KiB 块减少封装与往返，非末块按 Flash 的 4 KiB 擦除粒度对齐。方案复用现有 EV-MUX 帧和所有权，不增加物理连接、公开上传 API 或传输窗口。

2026-10-16，P4X-EYE revision 3.2 使用 ESP-IDF 6.1 固件 ``af41759`` 与 SDK 0.2.0 开发源码，在现有 Flash 文件系统得到以下上传耗时。计时包含 begin、数据与校验提交；每次上传后另外在设备端完整读取文件计算 SHA-256，全部一致。这些是当前设备实测样本，不是所有开发板的吞吐保证。

.. list-table:: P4X-EYE Flash 上传样本
   :header-rows: 1
   :widths: 25 25 25 25

   * - 接口
     - 256 KiB 旧写入器
     - 256 KiB 二进制上传
     - 2 MiB 二进制上传
   * - USB-Serial-JTAG
     - 40.94 s
     - 7.24 s
     - 55.75 s
   * - USB-OTG
     - 40.75 s
     - 7.01 s
     - 55.55 s
   * - TCP / Wi-Fi
     - 92.33 s
     - 9.26 s
     - 62.17 s

256 KiB 的数据 RPC 从 292 次降为 16 次，另加 begin/commit；应用层收发总字节从文件大小约 2.14 倍降至 1.04 倍。本机 Flash/FAT 顺序写入基线约为 55 KiB/s，这是当前存储实现的基线，不是 USB 物理带宽上限。小文件仍需承担 begin/commit 与 FAT 元数据的固定成本。进一步加速应继续测量存储与接收调度，而不是继续放大 JSON 分块。

公开文件 API 与线上实现
-----------------------

.. list-table:: 文件接口分层
   :header-rows: 1
   :widths: 30 36 34

   * - 主机操作
     - 线上实现
     - 使用场景
   * - ``device.files.write({ path, data, overwrite, ... })``
     - Core 在 user.rpc 使用 file.write 控制与原始二进制块，固件持续写原生 VFS，完整校验后提交；契约见上文。
     - 应用上传字符串或二进制字节的统一入口，不自动执行 Python。
   * - Device Core ``EvRepl.writeFile()``
     - framed REPL 执行 Python ``open`` / ``binascii.unhexlify`` / ``write`` / ``rename``。
     - 高级底层辅助方法，不是当前应用 ``files.write()`` 的实现路径，也不是名为 writeFile 的固件 RPC。
   * - ``device.files.read()`` / ``list()`` / ``remove()`` / ``mkdir()``
     - framed REPL 执行 ``seek/read`` / ``os`` 查询 / 删除，并解析受控输出。
     - 支持较大文件、结果校验与取消，仍要求 VM/REPL 可执行。
   * - ``debug.info`` 的 ``fs.list`` / ``fs.read``
     - ``debug.rpc``，受限 JSON/base64 调试响应。
     - 快速诊断；当前 fs.read 只读起始最多 384 字节，不是带分页 offset 的完整下载 API。

旧版 script.write 请求与分块
----------------------------------------

以下契约仅为 SDK 0.1.x 客户端保留，SDK 0.2.0 不再调用。

.. code-block:: json

   {"seq":20,"channel":"user.rpc","type":"req","method":"script.write","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

   {"path":"/hello.py","encoding":"utf-8","mode":"overwrite","offset":0,"totalBytes":11,"contentBase64":"cHJpbnQoMTIzKQo="}

示例内容是 11 字节的 ``print(123)\n``。真实客户端必须先生成源码 bytes，再把 ``len(source_bytes)`` 作为 totalBytes；下方示例以代码计算，避免手写长度误差。

.. code-block:: python

   import base64
   source = b"print(123)\n"
   payload = {
       "path": "/hello.py", "encoding": "utf-8", "mode": "overwrite",
       "offset": 0, "totalBytes": len(source),
       "contentBase64": base64.b64encode(source).decode("ascii"),
   }

``mode`` 可为 ``overwrite`` / ``create``，``encoding`` 可为 ``utf-8`` / ``binary``。offset/totalBytes 要么一起提供，要么一起省略。Base64 会把数据扩大到约 4/3，还要加上 path 和 JSON 结构；当前设备控制 payload 最大 2048 字节，不是可直接发送 2048 字节原始文件的容量。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     first [label = "offset=0\nprepare path.tmp"];
     next [label = "sequential chunks\nappend temporary file"];
     last [label = "offset + bytes = totalBytes"];
     publish [label = "rename temporary -> final path"];
     verify [label = "read back / size / content hash"];
     first -> next -> last -> publish -> verify;
   }

**图 17：script.write 在最后一块前使用目标路径的 .tmp 文件。** 首块会重新准备临时文件，后续块 append，最后一块提交 rename。未完成上传不应被主机报告为最终文件可用。

当前实现**不提供通用 offset 随机写或重复块自动去重**：后续非零 offset 实际执行 append，不应把任意 offset 解释为 seek。主机必须顺序串行发送、核对每个 ack 的 bytes/offset/totalBytes/complete；响应丢失后不要自动重复同一个 append 块，否则可能追加重复内容。

``complete:true`` 表示本次上传发布路径已走完，不包含整文件的密码学 hash 证明。重要文件要通过读回或受支持的设备端 hash 再校验内容。``runAfterWrite`` 只在 complete 后尝试把执行命令放入 REPL，不能证明应用启动或执行成功。

路径按 UTF-8 字节限制。当前 ``MICROPY_ALLOC_PATH_MAX`` 与 ``.tmp`` 后缀约束使脚本公开路径预算为 123 字节；还必须通过固件的允许字符检查。主机预检与最终固件检查都不能省略。

高级 framed REPL 辅助方法与公开读取接口
---------------------------------------

高级 ``EvRepl.writeFile()`` 辅助方法（不是 ``device.files.write()``）默认每块写入 224 个原始字节，以 hex 编码构造 Python 写入行，编码后的命令仍须满足 stdin 预算。默认写入 ``.viper.tmp``，关闭后移除旧目标再 rename；direct 模式直接写目标。这是客户端策略，与 RPC 的 ``<path>.tmp`` 不是同一个暂存约定，也不能被误写为通用跨掉电原子事务。

同一 REPL 上不应并行操作多个共享临时文件上传。取消或断线后，既要确认文件状态，也要清理仍开的目标端句柄/残留暂存；不能只清主机内存中的 promise。

读文件每次请求约 1 KiB 原始数据，目标端使用 hex 文本返还，并包含实际文件总大小与本块字节数。客户端验证总大小稳定、块长度与 hex 一致、是否过早 EOF，以及总期限；最后根据实际读回数据构造结果。一个短输出 burst 不能代表 EOF。

公开 ``device.files.read()`` 的默认总期限为 30 秒；IDE 可以为已实测的大文件/TCP 调度延迟使用有界的 60 秒预算。每个子请求仍需独立期限与取消检查；扩大总期限不等于允许无界阻塞或业务重放。

执行脚本不是上传 ACK 的延伸
---------------------------

.. code-block:: json

   {"seq":30,"channel":"user.rpc","type":"req","method":"script.run","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

   {"path":"/hello.py"}

.. code-block:: json

   {"ok":true,"path":"/hello.py","queued":true}

此响应只表示 ``execfile(...)`` 已排入 REPL 输入，不是 execution completed。应用还可能导入失败、缺模型、相机初始化失败、抛异常或进入无限循环。必须持续收包，等待应用级标记、真实预览或明确的共享执行生命周期结果。

应用使用 ``device.python.execute({ path, completion: 'start' })`` 进行交互运行，返回含 ``done`` Promise 的句柄。Core 内部委托给会话实现，应用不直接调用 ``startScript()``；普通无限运行不应套用有界试运行的自动停止期限。Run/Stop UI 根据共同的执行状态更新，不能覆盖 REPL 内部完成回调以猜测结束。

有界测试使用超时、取消和停止确认；交互应用持续运行，保活与 reader 继续工作。文件上传期间再次 Run 应被协调器拒绝/排队，而不是混入另一个脚本命令。

会话 lease 与存储 writer lease 是两件事
---------------------------------------

USB MSC 的主机块设备写入与 MicroPython VFS 共享同一卷，不能因为取得 EV-MUX session 就推定它可安全并行写入。``device.control`` 的 ``storage.writer.status`` / ``storage.writer.acquire`` / ``storage.writer.release`` 是独立的卷写入所有权接口。

它们涉及 volume（flash/sd）、operationId、writer leaseId 与当前 session，以及 available/held/handoff_ready/quarantined 等状态。交接时先关闭新 I/O admission，再等待进行中的操作/文件句柄 drain；异常条件可以隔离卷。主机应查询状态、执行其能力允许的安全交接；不能把 CDC 与 MSC 共存当作两个独立文件系统副本。

会话 token、writer leaseId 和网络 operationId 的字段格式虽可能相似，但作用不同，不能互换。writer 事务详细结构以 capabilities 与实际控制响应为准，不应把所有 file RPC 自动包装成已获得写权限。
