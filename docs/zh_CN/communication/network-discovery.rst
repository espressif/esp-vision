局域网发现协议与板级默认策略
============================

:link_to_translation:`en:[English]`

.. note::

   实现通过 DNS-SD 公布 ESP-VISION 服务，并为 Device Core 提供 Node 和浏览器网络桥发现适配器。固件构建/真机资格验证仍是独立发布关卡。现有 hello discovery 只在 transport 已连接后工作。

轻量协议选型
------------

首版采用 mDNS/DNS-SD，无需中心服务器，复用 IDF 5.5、6.0、6.1 manifest 已有的 ``espressif/mdns: ~1.1.0`` 依赖。DNS-SD 用 PTR 枚举服务，SRV 提供主机与端口，TXT 提供属性，A 记录提供当前 IPv4 地址。

.. list-table:: 实现取舍
   :header-rows: 1
   :widths: 20 40 40

   * - 协议
     - 成本与收益
     - 结论
   * - mDNS/DNS-SD
     - 已有组件及主机工具；缓存和命名冲突处理有标准约定。
     - 优先采用；注册一个服务，并限制 TXT 属性规模。
   * - 自定义 UDP 查询/应答
     - 专用应答器可能更小，但需新增端口、解析器，以及重试、缓存、多网卡规则。
     - 仅在实测资源不足时考虑；首版不同时维护两套发现协议。
   * - SSDP/UPnP
     - 引入另一套发现格式，完整 UPnP 接入还涉及设备描述。
     - 对已有 EV-MUX 服务没有明确优势。

固定的 mDNS 1.1.0 Kconfig 默认任务栈为 4096 字节，这不是全部堆内存占用。复用已有单实例，不新增发现任务/socket。需对比服务注册前后的镜像大小、内部堆剩余量/最低值和任务栈水位；目前尚无实测 RAM 增量。避免应用层持续发送心跳广播，复用组件的查询、公布和 TTL 行为。

服务约定
--------

* 在本地链路通过 mDNS UDP 5353 查询 ``_esp-vision._tcp.local``。
* 主机名：``esp-vision-<完整开发板名>-<MAC 后6位十六进制>-<固件版本>.local``，板名/版本转换为小写 DNS 标签并用连字符分隔。版本来自固件构建，绝不使用 IDE 版本。标签不超过 63 字节；长版本保留前缀并增加确定性的八位构建摘要，TXT 保留原始版本。所有板卡均使用主芯片 MAC。USB、USJ、UART、TCP 的 hello 事件/应答、capabilities 和设备 debug 信息返回相同的 hostname 标签（不含 ``.local``）及完整设备 ID。
* 实例显示名称为板型加标识后缀。SRV 公布实际 ``ESP_VISION_TCP_PORT`` （默认 3232），不在 TXT 重复端口。
* TXT 键：``txtvers=1``、``id=<device-id>``、``board=<board>``、``fw=<firmware-version>``、``evmux=4``、``hostname=<统一主机名>``。编码后的完整 TXT 限制为 256 字节，注册前检查长度。
* 复用 Launcher 从主芯片 eFuse MAC 派生的设备标识，Hosted Wi-Fi 板卡也遵循此规则。不能替换为无线协处理器 MAC、IP、boot ID 或 session ID。稳定标识不可用时报告发现不可用，不生成随机永久身份。
* 不公布密码、会话凭据或周期刷新的 owner 状态。发现属性不可信，不构成身份认证。

固件生命周期与 Python 共存
--------------------------

平台网络服务统一负责 mDNS 初始化，并协调当前会初始化 mDNS、设置 MicroPython hostname 的 ``overlay/micropython/ports/esp32/network_wlan.c``。不保留两个独立初始化标记。Python mDNS 查询复用共享实例。``network.hostname()`` 返回同一个统一名称；设置其他名称会抛出 ValueError，保证固件只有一个定义明确的身份。DHCP 和 Python port 也使用此名称，软重启不会重新生成。 ESP-IDF 网络接口的主机名上限对齐 DNS 的 63 字节；DHCP 选项空间设置为 255 字节，以同时容纳完整标签和其他选项。

仅在 STA 获得 IPv4 且 EV-MUX TCP listener 成功绑定后注册服务。listener 关闭、Wi-Fi 禁用或 STA 断开时撤销；重连后公布新地址。意外断电依赖主机 TTL 过期。软重启不能重复注册服务或停止平台发现应答器。mDNS 失败需可观测，但不能中断正常 TCP/USB 访问。 共享网络 sdkconfig 将 mDNS 限制在 STA 接口，与 TCP 绑定地址一致；AP/Ethernet 接口不公布此服务。

主机与 Device Core 对接
-----------------------

Device Core 统一发现结果约定及取消/超时语义，通过注入环境适配器执行网络枚举。Node 主机可直接使用 DNS-SD 适配器。普通网页无法枚举 multicast DNS，浏览器 IDE 通过本地 Node 网络桥获取结果。这不引入 Core 对 Agent 的依赖。

结果包括稳定设备 ID、显示名称、板型、固件、EV-MUX 版本、hostname、IPv4 地址、TCP 端口和网卡标识。按稳定 ID 聚合，同时保留不同网卡/地址候选；删除过期地址，保留有效备选。hostname 不能单独作为设备身份。查询有界，支持 AbortSignal，取消时释放枚举资源。

枚举阶段不逐台连接 TCP，也不申请所有权。用户选择后，Core 连接并发送不申请所有权的 hello，校验 v4、稳定设备身份及真实占用状态，再明确申请会话。申请及不申请所有权的 hello 应答均包含稳定的 ``deviceId``、``hostname``。传入 ``expectedDevice`` 后，``DeviceSession.negotiate`` 会先发送不申请所有权的 hello 核对这些字段，再申请会话。会话占用时提示用户，接管继续遵循现有显式策略。

网络桥只负责枚举和字节转发，EV-MUX 仍由 Core 处理。发现 HTTP/控制接口与字节桥采用相同的精确 Origin 和 loopback 策略。通过网络桥生成的不透明句柄选择已解析候选，并重新校验缓存地址/端口；保留显式固定目标的手动模式，不暴露浏览器任意指定 TCP 目标的转发能力。

IDE 在现有网络连接区域增加刷新和发现设备选择。保留手动 IP/hostname 输入，以应对 multicast 过滤、客户端隔离或不同 VLAN。本地网络访问权限本身不提供 DNS-SD 枚举能力。

Wi-Fi 板卡默认启用策略
----------------------

带 Wi-Fi 的板卡默认启用 WLAN、``ESP_VISION_ENABLE_BACKGROUND_NETWORK=1`` 和 ``ESP_VISION_ENABLE_TCP_TRANSPORT=1``。当前七种 Wi-Fi 配置已满足：``ESP32_P4X_EYE`` 和 ``ESP32_P4X_FUNCTION_EV_BOARD`` 使用 Hosted Wi-Fi；``ESP32_S3_EYE``、``AtomS3R-M12``、``ESP32_S31_CHATBOT``、``ESP32_S31_KORVO`` 和 ``ESP32_S31_MOSAICO`` 使用原生 Wi-Fi。两种 Hosted 板卡均包含 ``sdkconfig.p4_wifi_common``。

``ESP32_P4X_VISION`` 当前关闭 WLAN，且没有 Hosted Wi-Fi 配置。P4 本身没有原生 Wi-Fi，应先确认外接无线模组和板级连线，再启用该 port。``TEMPLATE`` 也依赖具体硬件。不能直接复制其他板卡的 Hosted SDIO/reset 引脚。

默认启用表示固件服务在启动时可用，无需 Python 脚本。出厂 SSID 为空时保持未配网，有效保存配置自动重连，保存的禁用/忘记意图仍被保留。TCP 仅在网络条件满足时监听。发现功能跟随 TCP listener 生命周期，采用同样的默认策略。

实现与验收
----------

1. 固件：统一 mDNS 生命周期，注册/撤销服务，在不申请所有权的 hello 中提供稳定身份。
2. Core：定义适配器/结果、取消语义及选择时身份核对；Node 和浏览器发现都与 EV-MUX 解析器保持职责边界。
3. IDE：扩展本地网络桥和连接面板，保留手动地址。
4. 验证多设备、不抢占已占用会话、主机多网卡、DHCP 地址变化、禁用/再启用、意外断电/TTL、软重启、取消发现及 multicast 受限时的手动回退。在 CI 运行完整固件矩阵，并在原生及 Hosted 真机测量资源增量。

Device Core 发现示例
--------------------

Node 安装可选 multicast-dns 适配器依赖；浏览器使用已有本地网络桥。通过 ``npm run network:bridge`` 启动（无需固定 IP），再在 IDE 网络面板刷新/选择设备。候选句柄最多 30 秒过期，选择过期后重连须先刷新。

.. code-block:: console

   npm install @esp-vision/device-core multicast-dns

.. code-block:: javascript

   import { createNodeDiscovery } from '@esp-vision/device-core/discovery/node';
   const devices = await createNodeDiscovery().list({ timeout: 3000 });
   // Optional: createNodeDiscovery({ interfaceAddress: '<local-interface-ipv4>' })

.. code-block:: javascript

   import { createBridgeDiscovery } from '@esp-vision/device-core/discovery';
   const devices = await createBridgeDiscovery('ws://127.0.0.1:8766/device').list();
   // Connect the chosen connectionUrl, then:
   // await session.negotiate({ role: 'tcp', expectedDevice: devices[0] });

协议参考
--------

* `Multicast DNS，RFC 6762 <https://www.rfc-editor.org/rfc/rfc6762.html>`_
* `DNS-Based Service Discovery，RFC 6763 <https://www.rfc-editor.org/rfc/rfc6763.html>`_
* `Espressif mDNS 1.1.0 Kconfig <https://github.com/espressif/esp-protocols/blob/mdns-v1.1.0/components/mdns/Kconfig>`_
