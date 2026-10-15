主机接入、Wi-Fi 配置与恢复实践
==============================

:link_to_translation:`en:[English]`

建议的主机栈
------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     ui [label = "IDE / Agent / CLI / custom tools"];
     session [label = "Device Core\ninspect() / configure() / control()"];
     parser [label = "EV-MUX v4 protocol\nCore framing / RPC / REPL"];
     transport [label = "byte-only host transport"];
     serial [label = "Web Serial / native serial"];
     bridge [label = "browser WebSocket\nlocal fixed-target byte bridge"];
     socket [label = "native TCP socket"];
     device [label = "device usj / usb / uart / tcp"];
     ui -> session -> parser -> transport;
     transport -> serial -> device;
     transport -> bridge -> socket -> device;
     transport -> socket;
   }

**图 21：共享 Core 持有协议状态，transport 只传字节。** 网络桥接不再次 acquire、重编码帧或执行 Python；否则会出现两个会话 owner 和两份请求关联逻辑。

``@esp-vision/device-core`` 在 ``host/device-core`` 维护，提供唯一共享协议实现。IDE、独立 Agent 和固件真机检查均使用 Core；Agent 工作流、模型提供方和 MCP 插件属于 Agent 主机应用，不属于 Core 或固件通信依赖。应用通过 ``createDevice()`` 获取设备接口，parser、framed REPL 与会话保活是内部实现，不需要主机再次组装。

完整 Node TCP、浏览器 Web Serial 和高层 API 示例见 :doc:`device-core`。真机测试统一运行包的 ``npm run test:hardware``，串口适配器仅转发字节；它不是另一套 Python EV-MUX 客户端。摄像头与 LCD 检查选择见 :doc:`validation`。

此适配器细节图展开统一的 :doc:`../architecture/index`：主机应用 → Device Core → EV-MUX → 设备固件。Core 持有协议/会话状态，原生串口、Web Serial、TCP 与 WebSocket 桥只传字节。DNS-SD 发现位于连接前，ROM 烧写另走 loader，不是 EV-MUX 镜像传输。

接入顺序
--------

1. 选中物理端点，核实设备/USB 拓扑，打开 transport。
2. 串口仅在首次请求前进行一次有界启动排空；TCP 立即 acquire。
3. 启动持续 reader，以新 nonce 的 hello 接管，验证 evMuxVersion 和响应关联，再保存 session。
4. 查询 capabilities 与 firmware identity，协商命令/REPL 大小和 timeout。
5. 按设备报告间隔启动保活；reader 在所有业务期间持续运行。
6. 等当前 epoch 的 vmReady，并在要执行 VM 工作时确认 REPL 可执行。
7. 串行协调文件、运行、停止、复位；接收日志和 preview。
8. 关闭前尽量释放 session，再关闭 transport、reader 和 pending requests。

串口启动排空为什么只做一次
--------------------------

USB 串口打开可能读到上一次连接的完整帧、残帧、旧 traceback 和 IDF 启动文字；“打开串口”不会触发新的 discovery hello。``serial.flush()`` 通常等待的是主机 TX，不是读取/清除设备响应。

Core 在连接开始时，通过同一个 parser 接收并处理目标 300 ms 的启动数据；这一步不建立业务会话。窗口结束仍有未完成的 header/body 时，连接以结构化错误失败并清理，而非随意截断后马上发命令。

排空只减少已读到的旧数据，不保证全部设备/USB/主机缓冲已空，也不诊断 USB completion 错误。后续请求绝不能每次清输入，否则会丢弃保活、preview 或业务响应。

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     open [label = "open serial once"];
     drain [label = "300 ms bounded read\none parser across chunks"];
     test [label = "no incomplete header/body"];
     hello [label = "new correlated hello/acquire"];
     work [label = "continuous reader\nnever flush per request"];
     open -> drain -> test -> hello -> work;
   }

**图 22：排空是接入准备，不是完成关联的替代品。** 后续仍需拒绝旧 seq/session/epoch。

统一真机入口
------------

在仓库根目录使用 Node.js 22 运行；串口模式还需要可执行 Python 与 ``pyserial``，TCP 模式不依赖 Python。下例先验证已联网设备的身份和摄像头，再对 USB 设备运行完整检查：

.. code-block:: console

   npm --prefix host/device-core run test:hardware -- --tcp <device-ipv4>:3232 --checks identity,camera
   npm --prefix host/device-core run test:hardware -- --serial <selected-port> --role usj --checks all --timeout 600000

测试会停止现有 Python 程序，并按所选检查初始化摄像头或显示测试图案。为设备安排独占测试时段，先关闭占用它的 IDE/终端；不会通过切换接口绕过有效 owner。详细检查集、LCD JPEG 输出和验收证据见 :doc:`validation`。

Wi-Fi 与 NVS 的真实状态机
-------------------------

第一次配网需要任一已取得会话的连接，通常使用 USB/USJ/UART；没有已配置网络时不能依赖这个 TCP server 做完全无线首次配网。Native/Hosted Wi-Fi 与 Python WLAN 共享底层实例，后台网络服务不会创建第二套 WLAN 驱动。

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Device [label = "network.configure + operationId"];
     Host <-- Device [label = "accepted; operation queued"];
     Device -> WiFi [label = "try candidate credentials in RAM"];
     WiFi -> Device [label = "got IPv4 / timeout / failure"];
     Device -> NVS [label = "persist only after got IP"];
     Host -> Device [label = "network.status"];
     Host <-- Device [label = "operation result + connection state"];
   }

**图 23：accepted 与连接成功、保存成功分别确认。** 网络修改在 accepted 回复 TX 完成后执行，最多等待约 1000 ms；configure 候选在 RAM 中尝试，约 30 秒内取得 IP 后才提交记录，失败恢复上一份保存的配置。

.. code-block:: json

   {
     "action": "network.configure",
     "operationId": "fedcba9876543210fedcba9876543210",
     "ssid": "<wifi-ssid>",
     "password": "<wifi-password>"
   }

通过 ``user.rpc/device.control`` 发送，metadata 携带当前 sessionId。网络控制不依赖 VM 空闲。``network.status`` 查询最终状态，``network.enable`` 控制保存的启用状态，``network.forget`` 保存忘记配置状态；不能将它们视为 Python ``WLAN`` 方法名直接塞进 RPC。

修改事务使用新的 32 字符 operationId；只在恢复同一事务时复用相同 ID 和完全相同 payload。事件 ``network.changed`` 可提示变化，但 status 是有界关联查询；重启/断线后不凭旧事件推断最新事务结果。

已有 NVS 记录优先，包括 disabled、forgotten 空记录和损坏记录；出厂宏不会覆盖它们。出厂默认仅在完全无记录时先保存再尝试连接；RPC 用户配置则先取得 IP 后保存，这是两种不同来源的流程。

后台事务期间，Python WLAN 修改会返回 EBUSY；普通 Python WLAN 修改可临时影响运行状态，但不隐式改写后台持久记录。关闭后台开关仍可显式使用 Python WLAN，不过 TCP 后台服务和其配置策略也随服务边界变化。

公开示例使用占位凭据，主机日志与诊断应脱敏。TCP 本版没有认证/加密，配置操作按其受信任开发网络的实现边界使用。

浏览器网络接入
--------------

浏览器不能直接打开任意 TCP socket。IDE 的固定目标本机 WebSocket-to-TCP bridge 将二进制字节送到设备 TCP；它只允许显式 origin、固定设备地址、单个活动浏览器，并在浏览器断开时关闭 TCP。它本身不解释 EV-MUX 或 acquire。

主机桥接程序位于独立 IDE 仓库，不能在固件根目录直接假定有相同 npm 脚本：

.. code-block:: console

   npm run network:bridge -- --device <device-ipv4>

本地 IDE 通常使用 ``http://127.0.0.1:8766/`` 与 ``ws://127.0.0.1:8766/device``。远端 HTTPS 页面需显式允许精确 origin，并满足浏览器本地网络访问权限/安全策略；需要 TLS 时使用受信任的 WSS 代理。USB Web Serial 与网络桥共用 ``createDevice`` 和字节传输契约。

复位与取消的确认
----------------

软复位：等待同 owner 的新 vmEpoch、vmReady 和 REPL barrier；异步 event/hello 有独立观察预算，缺事件本身不是复位失败。有事件本身也不证明 REPL 已可执行。

硬复位/ROM：废弃旧会话与请求；等物理枚举/固件恢复，重新接管并核对 firmware identity。主机烧写器的验证/复位状态应分开记录，已完整读回验证但未观察到运行态恢复，不应自动重复写镜像。

取消：停止继续上传/执行，尽可能中断目标工作并确认目标状态；若写入或执行是否完成不可知，保留“未知”结论。网络事务可按其 operationId 关联；普通业务 RPC、REPL 输入和文件 append 没有通用自动重放。

主机超时预算不能共用一个数字
----------------------------

* discovery/capabilities：参考客户端的关联 RPC 默认预算约 6 秒。
* keepalive：单次约 3 秒，同时满足固件 lease 总预算。
* 串口 startup drain：目标 300 ms，首次连接一次。
* 主机 RX 拼帧：参考实现约 12 秒预算，允许 TCP 收包出现数秒间隔。
* 设备 RX：空闲/整帧约 3/10 秒，与主机收大图预算不是同一个方向。
* VM 排队：设备约 5 秒过期，增加主机等待不扩大设备队列。
* 文件 readFile：Core 默认总期限 30 秒；IDE 实测场景使用有界 60 秒。
* Wi-Fi 配置：事务尝试约 30 秒；accepted 回复只是开始，须另查询最终状态。

这些是当前默认/参考实现值，运行前以实际 capabilities 和所用客户端选项为准。只对具有明确幂等恢复语义的操作设计有界重试；不要因网络慢就无限扩大等待或自动执行第二次应用命令。
