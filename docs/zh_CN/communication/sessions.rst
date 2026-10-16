会话所有权、接管与恢复
======================

:link_to_translation:`en:[English]`

“抢占”在 v4 中的准确含义
------------------------

所有支持的物理入口都能争用接管资格，但只有设备**先受理**的合法接管请求成为 owner。当前 owner 有效时，后来请求不会强制抢走控制权；默认口、DTR、USB 插入顺序、debug stream 都不是抢占优先级。

``route.bind`` 和 ``route.auto`` 已停止作为公开操作，返回 ``UNSUPPORTED_OPERATION``。user/debug 两个 stream 随唯一 owner 一起交接。要让主机 B 接手，应由 A 释放，或等待 A 的 link 失效/租约到期，然后 B 重新接管。

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     A -> Device [label = "hello: acquire=true, nonce A"];
     A <-- Device [label = "session S; user/debug -> A"];
     B -> Device [label = "hello: discovery only"];
     B <-- Device [label = "identity/capabilities; no owner token"];
     B -> Device [label = "hello: acquire=true, nonce B"];
     B <-- Device [label = "SESSION_BUSY; TCP competitor closed"];
     A -> Device [label = "session.release(S)"];
     A <-- Device [label = "release response"];
     Device -> Device [label = "TX completion; release owner"];
     B -> Device [label = "new link / nonce; acquire"];
     B <-- Device [label = "new session; user/debug -> B"];
   }

**图 9：发现、竞争失败与显式交接。** B 能发现设备，不表示 B 可发送 Python 或文件命令；旧 owner 的 token 也不能交给 B 使用。

会话状态机
----------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     free [label = "FREE\nno owner"];
     owned [label = "OWNED\nhandshake + lease"];
     releasing [label = "RELEASING\nrelease reply pending"];
     free -> owned [label = "valid acquire"];
     owned -> owned [label = "same link + same nonce"];
     owned -> releasing [label = "release prepare"];
     releasing -> free [label = "reply TX complete"];
     owned -> free [label = "expire / link loss / TX failure"];
   }

**图 10：释放分成准备与完成，避免在回复发出前销毁回复所需的 link。** 已收到释放回复后立刻重接管，仍可能短暂遇到 ``SESSION_RELEASING``。同 link 的主机可在总期限内用相同 nonce 重试该次 hello；不能把它变成无限自动重试。

首次发现与接管请求
------------------

串口打开后先处理启动残包；TCP 直接接管。以下代码块中的 metadata 与 payload 是同一个帧的两段内容，不是两个独立 JSON 帧：

.. code-block:: json

   {"seq":1,"channel":"user.rpc","type":"req","method":"hello","encoding":"json"}

   {"evMuxVersion":4,"acquire":true,"clientNonce":"0123456789abcdef0123456789abcdef"}

以下为省略附加能力字段的成功 payload 示例：

.. code-block:: json

   {
     "ok": true,
     "evMuxVersion": 4,
     "session": {
       "id": "89abcdef0123456789abcdef01234567",
       "owner": "tcp",
       "leaseMs": 15000,
       "keepaliveMs": 5000,
       "vmEpoch": 1
     },
     "vmReady": true,
     "routes": {"user": "tcp", "debug": "tcp"}
   }

必须先正确匹配 ``rsp/hello`` 的 seq、channel、method，再接受新 token。空 hello / capabilities 只是发现；发现响应回到其准确入站 link，不把有效 owner 的 ID 暴露给竞争连接。

``clientNonce`` 是一次逻辑接管的 32 字符小写十六进制标识。只在同一 link 重试同一接管时复用它；新 socket、重新打开串口或新一轮接管应生成新值。同 link、同 nonce 的幂等 hello 可以返回同一有效会话，不能用于透明重放其他业务。

设备在接管回复 TX 完成前还有握手完成门控，不能因为某个生产者已生成 sessionId 就在主机收到有效回复前发送依赖会话的操作。

保活不是应用完成确认
--------------------

.. code-block:: json

   {"seq":2,"channel":"user.rpc","type":"req","method":"session.keepalive","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567"}

   {}

默认每 5000 ms 保活，租期 15000 ms；合法、已授权的业务活动也续租。主机应按设备报告值调度，并在长脚本、长文件传输和持续预览时继续读包及保活，不能“等当前操作完成再续租”。

保活成功只表示当前 link/session 有效，并可报告 VM 代次与就绪；它不证明脚本执行完成、文件已完整发布或某次网络配置已生效。

四个身份/关联维度
-----------------

.. list-table:: 不可互换的字段
   :header-rows: 1
   :widths: 22 42 36

   * - 维度
     - 作用
     - 不可推导的事实
   * - link generation
     - 拒绝旧物理连接残留的工作。
     - 同一端口名或 TCP slot 不保证仍是同一次连接。
   * - ``sessionId``
     - 授权某个准确 link 持有当前会话。
     - 不能从发现响应、其他主机或旧 socket 转移过来。
   * - ``vmEpoch``
     - 对 VM 操作与 VM 输出标识代次。
     - 代次前进不等于 VM 或 REPL 已空闲可执行。
   * - ``seq`` + channel + method
     - 在一个请求上下文中关联响应。
     - 相同 seq 的 event、preview 或旧响应不能完成当前 RPC。

所有非发现业务携带 sessionId。绑定 VM 代次的 ``file.write`` / ``script.write`` / ``script.run``、相机/对象/VFS 调试、``repl.stdin`` / ``repl.signal`` 还携带当前 vmEpoch。传输统计、保活、释放和后台网络控制不必依赖 VM 空闲，但仍要求相应的 owner 授权。

启动 VM-ready 与软复位
----------------------

当前初始 ``vmEpoch`` 是随机非零 uint32；较早的 v4 构建可能在启动阶段报告 0。传输控制应保留实际返回值，VM 操作则等待正数 epoch 与 ``vmReady:true``。非零序号从 4294967295 回绕到 1，主机按序号比较规则判断更新，不能简单要求新数值大于旧数值。

软复位可以保持原 sessionId 和网络；新的 epoch 在复位开始推进。``event/hello`` 是额外通知，不能只靠它判成功：应使用关联的 keepalive 回复确认当前会话的新 epoch/ready，再执行当前代次的 REPL barrier，要求真实的新标记。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     old [label = "session S, epoch E\nold request / output"];
     begin [label = "soft reset begins\nepoch becomes E+1"];
     wait [label = "vmReady=false\nold queue admission stopped"];
     ready [label = "vmReady=true\nREPL may still run main.py"];
     proof [label = "interrupt if needed\nnew REPL marker"];
     old -> begin -> wait -> ready -> proof;
   }

**图 11：传输存活、VM 就绪和 REPL 可执行是不同条件。** ``boot.py`` 或 ``main.py`` 重新运行后仍可能占用 REPL；不能仅看到 ready 就立即开始 VFS/相机 VM RPC。

硬复位/进入 ROM 则必须废弃旧 token、epoch、待响应请求和解码器状态，恢复运行态后重新发现/接管。软复位时只重置旧 VM 的输出解析状态，不能把正在拼接的整条物理 EV-MUX 字节缓冲随意清空。

断线、FIN/RST、静默消失
-----------------------

* TCP FIN/RST 或已确认 link 失效：回收对应 owner，旧 generation 的输出失效；创建全新连接与 nonce，再接管。
* USB 真实断开：link 失效。仅关闭主机串口、改变 DTR 不保证设备得知句柄已关闭，应显式释放。
* USJ/UART 静默消失：等租约回收 owner；端口类型和 default/fallback 不授予提前接管权。
* 曾持有会话的 TCP link 失去 owner：仍可能保留 30 秒 idle 窗口，然后回收 slot；此预算与 15 秒租约不同。

租约释放不会自动撤销已经完成的外部副作用，也不能视为“前一个用户程序保证已经停止”。B 接管后应确认 VM 状态，按需要显式中断，并用新标记确认 REPL 可执行。

错误与恢复决定
--------------

.. list-table:: 常见错误
   :header-rows: 1
   :widths: 30 70

   * - 错误
     - 主机处理
   * - ``SESSION_BUSY``
     - 当前另一个 owner 有效；提示释放/等待，而不是抢占或重用其 token。
   * - ``SESSION_RELEASING``
     - 同 link 释放完成边界尚未结束；仅在有界接管流程内重试同 nonce。
   * - ``SESSION_REQUIRED`` / ``NOT_OWNER``
     - 停止发送依赖旧会话的业务，废弃本地会话并重新建立。
   * - ``VM_EPOCH_MISMATCH``
     - 原 VM 请求不再有效，重新确认当前代次；不要自动重放脚本或写文件。
   * - ``VM_BUSY`` / ``VM_TIMEOUT``
     - VM 请求无法入队或未及时处理；先处理当前脚本、取消/复位与真实 REPL liveness。
   * - 链路发送失败 / 主机等待超时
     - 可能已产生副作用，标记完成情况未知；先查询或核验文件/事务状态。

带相同 ``operationId`` 的网络事务有专门的关联与恢复语义；``seq`` 本身不是所有 RPC 的幂等键。脚本执行、REPL 输入和文件 append 没有通用透明重放契约。
