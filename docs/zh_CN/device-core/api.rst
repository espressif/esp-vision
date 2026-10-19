应用 API 完整参考
================================

:link_to_translation:`en:[English]`

本页覆盖正在准备、此改动尚不发布的 0.4.0 中 ``createDevice`` 返回对象的全部应用 API。除特别说明外，操作异步执行，失败时 reject。请求中的未知字段会被拒绝；不能给所有方法统一附加未声明的 ``timeout`` 等选项，应以各方法列出的字段为准。超时单位为毫秒，须为有限正数。

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 方法
     - 用途
   * - ``createDevice(options)``
     - 创建一个独立管理连接的设备对象。
   * - ``device.discover(options)``
     - 枚举局域网公告，不获取设备所有权。
   * - ``device.connect(options)``
     - 打开连接、协商 v4 会话并验证 REPL 可用。
   * - ``device.disconnect()``
     - 释放所有权、取消工作并关闭连接。
   * - ``device.inspect(request)``
     - 查询身份、能力、运行态、网络或存储。
   * - ``device.configure(request)``
     - 局部修改网络配置。
   * - ``device.control(request)``
     - 停止、复位或进入 ROM 下载模式。
   * - ``device.firmware.check(request)``
     - 检查开发板推荐固件版本。
   * - ``device.firmware.update(request)``
     - 下载、校验、烧写并回读 application 镜像。
   * - ``device.capture(request)``
     - 获取摄像头或 LCD 的 JPEG。
   * - ``device.on(event, handler)``
     - 订阅事件并获得退订函数。
   * - ``device.python.input(data)``
     - 发送交互式 REPL/stdin 输入。
   * - ``device.python.execute(request)``
     - 执行源码或设备中已有脚本。
   * - ``device.files.list(request)``
     - 列出目录。
   * - ``device.files.read(request)``
     - 读取字节或 UTF-8 文本。
   * - ``device.files.write(request)``
     - 创建或覆盖文本/二进制文件。
   * - ``device.files.remove(request)``
     - 删除一个文件或空目录。
   * - ``device.files.mkdir(request)``
     - 创建目录，可递归创建父目录。

创建对象与字节传输
--------------------------------

``createDevice({ transport?, discovery? } = {}) → device``

``transport`` 提供原始字节连接；``discovery`` 提供 ``list({ timeout, signal }) → Promise<Array>``。创建时二者均可省略，连接时必须具备 transport。仅发现用途的对象可在 ``connect`` 时补入 transport。Core 不会自动选择串口或根据发现记录建立 socket。

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 传输成员
     - 契约
   * - ``writeBytes(bytes)``
     - 必需；接收 Uint8Array，全部字节被宿主连接接受后 Promise 才完成。短写或失败必须报错。
   * - ``subscribe({ data, error, close })``
     - 必需；同步返回退订函数，data 接收原始 Uint8Array 字节块。
   * - ``connect({ signal })``
     - 可选；Core 完成订阅后打开连接。
   * - ``disconnect()``
     - 可选；关闭物理连接并释放 reader/writer 资源。

device.discover(options)
------------------------------------------------

``{ timeout = 3000, signal? } → Promise<DiscoveredDevice[]>``

``timeout`` 为 50–10000 的整数。必须配置 discovery provider，否则抛出 ``DISCOVERY_UNAVAILABLE``。结果会校验并按主机名排序，最多接受 256 条。发现不连接设备，也不代表设备当前没有 owner。

.. code-block:: javascript

   const discovered = {
     deviceId: 'e8f60ae75642',
     hostname: 'esp-vision-esp32-p4x-eye-e75642-2026-08-27',
     board: 'ESP32_P4X_EYE', firmwareVersion: '2026.08.27',
     evMuxVersion: 4, compatible: true, port: 3232,
     addresses: ['192.168.1.107'],
     // Optional: connectionUrl (bridge), expiresAt (expiry timestamp).
   }

上述为数据结构示例，不是应当连接的固定设备。``deviceId`` 是去掉分隔符的主芯片 MAC；``firmwareVersion`` 是固件版本，不是 IDE 版本。根据所选地址/端口创建字节连接，再将记录传为 ``expectedDevice``。Node 与浏览器 provider 见 :doc:`advanced`。

device.connect(options)
----------------------------------------------

``{ transport?, role = "unknown", expectedDevice = null, timeout = 30000, signal? } → Promise<Inspection>``

``role`` 接受 ``unknown``、``usj``、``usb``、``uart``、``tcp``。Core 先订阅再打开连接，协商 v4，按需检查 ``expectedDevice``，获取会话并探测 REPL。返回默认查询结果 identity/capabilities/runtime。正在连接、已连接或正在断开的对象会以 ``DEVICE_BUSY`` 拒绝重复连接。身份匹配不等于认证。连接可能中断既有 Python 程序，不是无副作用的在线探测。

device.disconnect()
--------------------------------------

返回 Promise。释放会话、终止待处理工作、关闭会话解释器、退订输入并调用可选的 transport 关闭方法。重复调用共用清理过程，已断开的对象直接完成。释放或传输清理失败仍可能 reject，因此连接失败也应在 ``finally`` 中清理。重连需要可重新打开的 transport，或通过 ``connect`` 提供新 transport。

device.inspect(request)
----------------------------------------------

``{ fields = ["identity", "capabilities", "runtime"], timeout = 5000, signal? } → Promise<Inspection>``

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 字段
     - 结果
   * - ``identity``
     - ``{ deviceId, hostname, board, firmwareVersion } | null``
   * - ``capabilities``
     - 协商得到的固件能力、features、channels、limits。
   * - ``runtime``
     - 会话/parser/transport 快照，附 vmReady、vmEpoch 和当前 run 或 null。
   * - ``network``
     - network.status 结果，包括 enabled/configured/state/IPv4 与事务状态，不含已保存密码。
   * - ``storage``
     - flash 与 sd 的 storage.writer.status 结果，要求 storage.writer.lease 能力。

``fields`` 必须是上述五个名字组成的非空数组，仅返回请求的键，重复字段只查询一次。身份和能力是协商数据的副本，不是新采集的传感器数据。网络与存储会发起 RPC；该超时应用于各底层查询，不是整个 inspect 的统一总时限。

device.configure(request)
--------------------------------------------------

``{ network: { enabled?, credentials? }, timeout = 45000, signal? } → Promise<{ ok, completed, network }>``

``credentials`` 为 ``{ ssid, password }`` 或 ``null``。SSID 为 1–32 个 UTF-8 字节，不得包含 NUL。密码可为空（开放网络）、8–63 个可打印 ASCII 字符或 64 位十六进制 PSK。``enabled`` 为布尔值。至少提供一个更新字段，省略字段保留原设置。清除凭据会关闭网络，不能同时设置 ``enabled: true``。

.. code-block:: javascript

   await device.configure({ network: {
     credentials: { ssid: 'example', password: 'example-password' },
     enabled: true,
   } })
   await device.configure({ network: { enabled: false } })
   await device.configure({ network: { credentials: null } })

配置总时限包含轮询事务最终结果，不只等待 accepted ACK。``completed`` 包含已确认的 ``{ action, operationId }`` 步骤。失败时附加 ``error.completed``，必要时附 ``error.pending``；已完成步骤不自动回滚。局部配置由顺序操作组成，不是原子事务。修改 Wi-Fi 可能断开 TCP，建议通过 USB 配网，不得记录凭据。

device.control(request)
----------------------------------------------

``{ action, mode?, timeout = 15000, signal? } → Promise<control result>``

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 请求
     - 行为
   * - ``{ action: "stop" }``
     - 中断/停止 Python，可在 python.execute 运行期间调用。
   * - ``{ action: "reset", mode: "soft" }``
     - 软重置 VM，观察新 epoch 和就绪状态。
   * - ``{ action: "reset", mode: "hard" }``
     - 重启硬件，连接可能消失。
   * - ``{ action: "bootloader" }``
     - 请求 ROM 下载模式，随后断开并使用 ROM 烧写器。

``mode`` 仅在 reset 时必需，其他动作提供此字段会报错。stop ACK 不代表脚本清理已完成，应继续观察 ``done``。复位/下载 ACK 不代表正常启动或 ROM 枚举已完成，设备恢复后须重连并查询。

device.python.input(data)
--------------------------------------------------

接收字符串或 Uint8Array，返回底层写入 Promise。交互行以 ``\r`` 提交；在 ``completion: "start"`` 的运行期间可向 stdin 写入（例如响应 Python ``input()``）。此方法没有请求对象形式的 timeout/signal 参数。输出由 ``python.output`` 事件交付，输入发送完成不代表求值成功。其他自动操作占用解释器时，冲突的终端输入以 ``DEVICE_BUSY`` 拒绝。

device.python.execute(request)
------------------------------------------------------------

``{ source? | path?, completion = "wait", timeout = 10000, maxOutputChars?, signal? }``

``source``（字符串）与 ``path``（设备绝对路径）必须且只能提供一个。``completion: "wait"`` 返回 ``{ ok, path, output, outputTruncated }``，直接执行 source 时 path 为 null。``completion: "start"`` 返回 ``{ ok: true, path, done }``，done 完成后返回同样的结果结构。``timeout`` 仅允许用于 wait 模式。``maxOutputChars`` 为非负整数，限制保留的输出字符数，不限制串口带宽或脚本运行时长。

.. code-block:: javascript

   const off = device.on('python.output', text => terminal.write(text))
   try {
     const run = await device.python.execute({
       source: 'name = input("Name? "); print(name)', completion: 'start',
     })
     await device.python.input('example\r')
     console.log(await run.done)
   } finally { off() }

示例 terminal 由 UI 提供。start 模式的 source 会上传到 Core 拥有的唯一临时文件，运行完成后清理；传入的用户 path 不会因此被删除。取消或断线可能使完成状态未知或清理未完成。须检查输出中的 Python traceback，``ok`` 是执行/传输结果，不能通用地断言任意 Python 程序获得了正确业务结果。

设备文件操作
--------------------------------

设备路径必须是绝对路径，最多 123 个 UTF-8 字节。禁止控制字符、反斜杠、双引号、任意位置的 ``..``、重复斜杠及尾随斜杠。只有 list 可使用根路径 ``/``。操作要求文件系统已挂载、存储所有权允许且空间足够。以下方法与 Python 共用解释器互斥。

device.files.list(request = {})
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path = "/", recursive = false, signal? }``

返回平铺数组 { name, path, type: "file" | "directory", size }，目录 size 为 0；递归结果仍是平铺结构。

device.files.read(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, encoding = "binary", timeout = 30000, maxBytes = null, signal? }``

binary 返回 Uint8Array，utf-8 返回字符串。maxBytes 为 null 或 0..16777216 的整数，限制整文件读取；超限会拒绝，不会悄悄返回文件前缀。

device.files.write(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, data, overwrite = true, onProgress = null, signal? }``

data 为字符串或 Uint8Array；overwrite:false 请求只创建。进度回调接收 { writtenBytes, totalBytes }。完整文件通过固件 SHA-256 校验和发布后，返回包含总 bytes、complete:true 与 path 的提交确认；不会执行文件。

device.files.remove(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, type = "file", signal? }``

type 为 file 或 directory；删除目录要求目录为空。返回 Promise<void>，不提供递归删除标志。

device.files.mkdir(request)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``{ path, recursive = false, signal? }``

返回 {ok:true}；递归模式创建父目录，允许目标已是目录，但不允许被同名文件占用。

SDK 0.2.0 及以上的所有文件（包括 Python 源码和模型）共用可靠的原始二进制 ``file.write`` 上传。固件须支持 ``file.write.binary`` 并提供有效 ``limits.fileWriteChunkMax``（4096..16384，按 4096 对齐）；不支持时返回 UNSUPPORTED_OPERATION 并提示更新固件，不回退到 Base64/JSON 或 REPL 上传。list/read/remove/mkdir 仍使用 framed REPL，无需另加 files.writeBinary 公开方法。进度是已确认接收的字节，最终 Promise 成功才代表完整长度/哈希校验和发布完成。取消/失败会尽力 abort，固件也会清理超时、旧会话或旧 VM 上传。传输、哈希和普通发布前失败保留原目标；FAT 覆盖使用旧文件备份与普通重命名失败回滚，不承诺掉电原子事务。线上契约见 :doc:`../communication/python-files`。应用更新使用 ``device.firmware.update()``，复用 ``/firmware`` 子路径的 ROM 烧写实现。

device.capture(request)
----------------------------------------------

``{ source, format = "jpeg", quality?, timeout = 15000, signal? } → Promise<{ data, mimeType, metadata }>``

``source`` 为 ``camera`` 或 ``screen``，格式只支持 ``jpeg``。可选 quality 为 1–100 的整数，省略时采用固件默认值。data 为 Uint8Array，mimeType 为 image/jpeg，尺寸位于 metadata.width/height，而非返回对象顶层。

.. code-block:: javascript

   const frame = await device.capture({ source: 'screen', quality: 80 })
   console.log(frame.metadata.width, frame.metadata.height, frame.data.length)
   const blob = new Blob([frame.data], { type: frame.mimeType })

摄像头抓图在 VM 中初始化/使用摄像头；LCD 抓图要求 LCD 已初始化，只读取最后提交的软件 RGB565 帧，不启动摄像头或重新绘制屏幕。不支持硬件返回 NOT_SUPPORTED，LCD 未初始化返回 DISPLAY_NOT_READY。软件抓图不是物理面板回读。按需缓存、DMA 完成同步与 preview 交付见 :doc:`preview-debug <../communication/preview-debug>`。

device.on(event, handler)
--------------------------------------------------

同步返回 ``unsubscribe()``。需要启动输出时应在 connect 前订阅。监听器抛错不会中断解析或其他监听器，会产生 listener.error 诊断事件。UI 销毁时须退订并释放图像 object URL。

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - 事件
     - 载荷
     - 含义
   * - ``python.output``
     - ``string``
     - Python stdout/stderr、提示符与交互输出。
   * - ``raw``
     - ``string``
     - EV-MUX 帧之外的文字，例如早期启动输出。
   * - ``log``
     - ``string``
     - 固件/IDF 诊断文字。
   * - ``preview``
     - ``{ data, mimeType, metadata }``
     - 应用 img.flush() 产生的 JPEG，允许丢帧。
   * - ``state``
     - ``{ state, identity?, reason? }``
     - connecting/ready/disconnected，以及 hello/network.changed 提示；ready 时附 identity。
   * - ``diagnostic``
     - ``{ event, ...details }``
     - rpc.request/response/timeout、listener.error 与清理诊断；网络敏感凭据会脱敏。
   * - ``rpc``
     - ``{ channel, metadata, payload }``
     - 高级入站 RPC 观察，业务操作优先使用设备方法。

互斥、取消与错误
--------------------------------

查询、配置、控制、Python、文件和抓图方法要求连接处于 ready。发现、事件订阅、固件检查不要求连接；固件更新指定开发板及 ROM loader 后也可在未连接时执行。设备对象遇到 Python、文件、配置、抓图等互斥操作冲突时返回 DEVICE_BUSY，不会自动把任意业务请求排队。stop/reset 可以与正在执行的 python.execute 协调；start 运行持续占用至 done，运行接受 stdin 时允许交互输入。

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 错误
     - 处理
   * - ``TypeError / RangeError / INVALID_PATH``
     - 修正参数；未知字段、非法枚举和路径会被拒绝。
   * - ``DEVICE_NOT_READY / DEVICE_BUSY``
     - 先连接，或等待/停止当前操作。
   * - ``DISCOVERY_UNAVAILABLE / UNSUPPORTED_OPERATION``
     - 补充 provider，或使用具备能力的固件。
   * - ``SESSION_BUSY / SESSION_LOST / VERSION_MISMATCH``
     - 释放其他有效 owner、会话丢失后重连或升级固件，不强制抢占或盲目重放写入。
   * - ``TIMEOUT / AbortError / CONNECTION_CLOSED``
     - AbortSignal 可能保留调用方 Error 原因；完成状态可能未知，重试修改前须核对身份、状态与文件。
   * - ``VM_BUSY / VM_TIMEOUT / DISPLAY_NOT_READY / NOT_SUPPORTED``
     - 明确处理固件错误，停止/初始化所需资源或提示硬件不支持。

所有异步调用均须处理 rejection，错误可能包含 code、errno、completed、pending，但不是所有错误类型都有 code。断开/取消不回滚设备修改、不保证脚本已终止，也不意味着可以重放非幂等操作。

固件检查与更新
------------------------------------------

``device.firmware.check({ catalog?, identity?, version?, mode = 'update', fetch = globalThis.fetch, signal? })``

返回 ``{ available, currentVersion, release, releases }``。已连接时使用真实设备身份；未连接时传入此前查询得到的 ``identity``，包含 ``board`` 与 ``firmwareVersion``。检查不会接管连接、复位或写入设备。``catalog`` 接受内存对象或绝对 HTTPS JSON 地址，下载失败会 reject；无匹配开发板时返回 ``release: null, available: false``。省略 catalog 时，Core 自动读取 dl.espressif.com 上的 ESP-VISION 官方目录及各版本更新元数据；自定义 catalog 只是高级覆盖。历史条目缺少更新元数据时仍可列出，但不可选为安装目标。

目录格式为 ``{ schemaVersion: 1, releases: [{ board, version, image }] }``，发布方按推荐的新版本在前排序；默认选择首个匹配开发板的条目并校验 v4 镜像元数据；指定版本时按该版本选择。``available`` 表示目标与当前固件版本不同，不表示任意日期/git 版本字符串数值更大。发布方负责推荐版本顺序，UI 在更新前展示目标版本，包括有意降级的情况。

``image`` 包含 ``evMuxVersion: 4``、非负整数 ``chipFamily``、``offset``，以及精确 ``chipRevision`` 或整数范围 ``chipRevisionMin``/``chipRevisionMax``，正整数 ``bytes`` 与 ``partitionSize``（镜像须放得下），64 位小写十六进制 ``sha256``，以及 ``partitionTable: { offset, bytes, sha256 }``。需要下载镜像时增加绝对 HTTPS ``url``，直接指向 application bin；zip/artifact 下载不是原始 bin，Core 不负责解压。开发板名必须与固件身份完全一致，版本号是固件版本，不是 SDK/IDE 版本。SHA-256 用于校验与所选目录一致，不能证明恶意发布源可信。

``device.firmware.update({ release, board?, mode = 'update', data?, fetch = globalThis.fetch, openLoader, reset?, onProgress?, onPhase?, signal? })``

``release`` 为检查结果中选择的条目。已连接设备必须匹配该开发板；未连接时必须通过 ``board`` 显式指定实际选择的开发板。``data`` 为 Uint8Array application bin；省略时下载 ``release.image.url``。Core 先校验尺寸和哈希，再释放 EV-MUX 连接，随后调用宿主的 ``openLoader({ signal })`` 获取 ROM 下载端口。宿主适配器负责串口/WebUSB 选择及 ROM 进入操作，Core 不实现网络 OTA，也不另造第二套进入下载模式的线上指令。即使通过 TCP 发现/查询设备，更新仍需要本地下载连接。

loader 契约沿用 :doc:`advanced` 中的 FirmwareFlashSession。可选 ``reset(loader)`` 替代其 hard-reset 策略，``onProgress(percent)`` 报告包含回读在内的 0–100 进度，``onPhase(text)`` 报告阶段。默认 update 模式仅写 application 分区：写入前校验芯片/修订版本、安全状态及现有分区表，写入后完整回读并校验哈希。分区不匹配会拒绝；默认更新不做整片迁移，不擦除 NVS 或用户文件；显式完整安装见下文。

完整回读校验成功后返回 ``{ verified: true, needsReset, version, board, reconnected: false }``。``needsReset: true`` 要求宿主提示用户物理复位。不自动重连 EV-MUX，也不宣称新版本已运行，须另行重连并查询固件版本。取消信号在下载、获取 loader 以及写入前有效；开始烧写后延迟取消，确保完成回读。失败会 reject 并关闭已获取的 loader，不承诺回滚。更新期间连接、其它设备操作及并发更新以 DEVICE_BUSY 拒绝。未知字段会拒绝。交接后设备保持断开，包括交接之后发生失败的情况。

.. code-block:: javascript

   const result = await device.firmware.check({
     catalog: trustedCatalog, // 宿主发布方提供的有序固件目录。
     // 离线/ROM 恢复时：identity: previouslyInspectedIdentity
   })
   if (result.available && userApprovedVersion === result.release.version) {
     const updated = await device.firmware.update({
       release: result.release,
       openLoader: ({ signal }) => host.openRomLoader({ signal }),
       onProgress: percent => console.log(percent),
     })
     console.log(updated) // 另行重连/查询；needsReset 时提示复位。
   }

空白设备首次安装
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

复用 ``firmware.check/update``，显式传入 ``mode: 'install'``。不需要 EV-MUX 连接或运行中的固件；用户必须选择真实开发板，使芯片进入 ROM 下载模式，并选择 ROM 支持的 USB-Serial-JTAG（USJ）或 UART 端口。空白设备不能依赖应用固件提供的 USB-OTG CDC 接口。浏览器宿主仍提供 ROM 适配器，CLI 内置 Node/esptool 适配器。此流程不是 TCP OTA。

默认 ``mode: 'update'`` 保留原有只更新应用的行为；分区不匹配时拒绝，不会自动切换到 install。显式 install 将经过校验的完整合并镜像从地址 0 写入，包括 bootloader、分区表和应用。镜像覆盖范围内已有数据（包括可能存在的 NVS、文件系统数据）可能被替换，不承诺保留或回滚，也不会隐式执行整片擦除。宿主界面应明确区分首次安装与保留数据的应用更新。

沿用 schemaVersion 1，发布条目新增可选 ``install: { offset: 0, bytes, flashSize, sha256, url, chipRevisionMin, chipRevisionMax, bootloader: { offset, bytes, sha256 } }``。flashSize 是布局要求的 Flash 容量（字节）；发布器取 bootloader 与应用芯片修订范围的交集。原有 image 仍是应用描述；安装会核验完整镜像中 bootloader、分区表和应用各自范围的哈希，范围必须有序、不重叠且不越界。发布流水线从现有合并镜像与实际 IDF 产物生成这些元数据；历史版本缺少合格安装元数据时不会推测兼容。

``firmware.check({ mode: 'install', ... })`` 选择第一个具备安装元数据的版本，或精确选择指定版本。仅提供应用更新的版本仍显示在 releases 中，显式选择它安装时报 FIRMWARE_INSTALL_UNAVAILABLE；未知 mode 被拒绝。``firmware.update({ mode: 'install', ... })`` 在释放会话和获取 ROM 端口前校验镜像，随后检查真实芯片、修订、安全启动/加密状态及 JEDEC 实测容量，再开始写入。空白设备没有分区表，因此安装不要求旧表匹配。完整回读、复位策略、互斥、取消与返回结构沿用应用更新；是否已运行新固件仍须重新连接并 inspect 确认。

.. code-block:: javascript

   const device = createDevice() // 不需要运行态 transport 或 connect()。
   const selected = await device.firmware.check({
     identity: { board: 'ESP32_P4X_EYE', firmwareVersion: 'unknown' },
     mode: 'install', version: selectedFirmwareVersion,
   })
   await device.firmware.update({
     release: selected.release, board: 'ESP32_P4X_EYE', mode: 'install',
     openLoader: ({ signal }) => host.openRomLoader({ signal }),
   })

列出并指定固件版本
------------------------------------------

``releases`` 返回目录中当前开发板的全部版本，保留发布方排序；省略 ``version`` 时，``release`` 是第一个推荐版本。传入 ``version`` 则按固件版本精确匹配，不存在时以 FIRMWARE_VERSION_UNAVAILABLE 拒绝，不会悄悄换成其它版本。同一开发板存在重复版本条目时拒绝该歧义目录。能查询的历史范围由发布目录决定，Core 不抓取所有 Git tag、CI 产物或未发布版本。显式选择旧版本也沿用相同的更新校验。

.. code-block:: javascript

   const history = await device.firmware.check({ catalog: trustedCatalog })
   console.log(history.releases.map(item => item.version))
   const selected = await device.firmware.check({
     catalog: trustedCatalog, version: selectedFirmwareVersion,
   })
   await device.firmware.update({
     release: selected.release,
     openLoader: ({ signal }) => host.openRomLoader({ signal }),
   })

Node 宿主可直接复用随包提供的 ROM 适配器：

.. code-block:: javascript

   import { openNodeRom } from '@esp-vision/device-core/node'
   await device.firmware.update({
     release: selected.release, board: 'ESP32_P4X_EYE', mode: 'install',
     openLoader: ({ signal }) => openNodeRom({ port: selectedRomPort, signal }),
   })

尚未发布的浏览器 ROM 适配器为 ``@esp-vision/device-core/browser`` 的 ``openBrowserRom({ port, logger?, signal? })``，复用上述安装/升级契约；用户手势串口授权和打包器要求见 :doc:`integration`。npm 0.3.0 尚不含此 API。

不可变发布快照与单 BIN 升级（0.4.0）
------------------------------------

此源码正在准备 SDK 0.4.0；发布是单独步骤。``loadOfficialFirmware`` 与 ``loadReleaseManifest`` 从 ``@esp-vision/device-core/firmware`` 导入；根入口仍只导出 ``createDevice``。

``loadOfficialFirmware({ board?, version?, fetch = globalThis.fetch, signal? })`` 优先读取 ``config.toml`` 的 ``[publication]``：``schemaVersion: 1``、``version``、40 位 ``sourceCommit``、``manifestUrl``、原始字节 ``manifestSha256`` 及 ``releases: [{ version, manifestUrl, manifestSha256, boards }]``。历史版本不重复，各板集非空且唯一；发布器的其他事务字段保留但不改变读取逻辑。指定板型/版本必须存在于该引用。仅下载所选版本的一份 manifest，不遍历历史板型 × 版本矩阵；``firmware.check({ version })`` 会传递版本给 reader。显式 publication、引用或 manifest 无效时明确失败，包括 404 或哈希不匹配，不作回退。只有完全不含 ``publication`` 的配置保留旧 feed；指定版本也会约束旧元数据请求范围。

``loadReleaseManifest({ url, sha256, version?, sourceCommit?, boards?, fetch = globalThis.fetch, signal? })`` 不读取可变 config，只读取引用的快照。SHA-256 覆盖 UTF-8 JSON 原始字节。结果包含 ``schemaVersion: 1``、``version``、``sourceCommit``、正整数 ``pipelineId``、``layout: 'legacy' | 'single-bin'``、现有合格 ``releases`` 数组、完整 ``website`` 快照及可选 ``mcp``（null 表示明确缺失）。可选身份/板集参数须完全匹配；返回前校验所有 release 与来源。官方 URL 限定在 ``https://dl.espressif.com/AE/esp-vision/``；模型下载固定到 ``raw.githubusercontent.com/espressif/esp-vision/<sourceCommit>/models/...``。MCP 描述含 ``url/bytes/sha256/version/sourceCommit``，使用官方 ``bin/mcp/`` 来源；website 版本/板型/模型也须有效，隐藏固件板型不要求出现在可视化板列表。读取限制为最多 64 个发布板型、256 个版本/模型记录、4 MiB manifest 字节。固定哈希检测相对可信发布引用的漂移，不等于为不可信发布者提供签名认证。

``loadOfficialFirmware`` 在 manifest 快照外返回 ``publication``；可选 ``board`` 只在整份 manifest 校验后筛选返回的固件条目。``publication.releases`` 提供合格历史版本/板型引用；``website.releases`` 可以保留缺少合格元数据的旧版本下载列表，不能因为可以拼出 BIN URL 就开放烧录。网站从固定引用构建后，应直接将该引用交给 ``loadReleaseManifest`` 选择版本，使界面与固件始终来自同一快照。

``layout: 'single-bin'`` 中每个 release 含 ``image.source: { url, bytes, sha256, offset }``；URL 同时等于 ``image.url`` 与完整镜像的 ``install.url``，长度/哈希等于 ``install.bytes/sha256``。``source.offset`` 等于 ``image.offset``，应用范围须在完整容器内；``image.bytes/sha256`` 仍仅描述应用。Core 一次下载整个 source，在获取 ROM 前拒绝长度/哈希/内部 bootloader、分区表或应用哈希不符，切出应用后复用设备安全、分区检查、写入与完整回读流程。分区布局变化仍拒绝默认升级，不自动改为安装。显式 ``mode: 'install'`` 写入并校验完整镜像；传入 ``data`` 也遵循描述，存在 ``image.source`` 时必须是完整容器，否则沿用原始应用 BIN。继续支持 ``legacy`` manifest 与旧应用独立描述。下载受合格精确长度及 256 MiB 上限约束，使用 HTTPS、拒绝重定向，暂不使用 HTTP Range。浏览器调用方无需自行下载、切片或实现校验。

固定哈希的 manifest 和完整 BIN source 使用浏览器 HTTP 缓存（``cache: default``）；可变 config 与旧独立应用 BIN 保持 ``no-store``。固定资产在 fetch 阶段遇到 TypeError 时，仅一次 ``no-store`` 重试以恢复缺少 CORS 头的旧缓存。HTTP、读取响应、SHA-256 或元数据错误均不重试、不降级。
