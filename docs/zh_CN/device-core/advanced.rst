高级导出 API 完整参考
========================================

:link_to_translation:`en:[English]`

本参考按 package.json exports 覆盖全部公开导入路径和命名导出，列出所有导出类不以下划线开头的公开原型方法。下划线方法及未通过包导出的 src/device_* 实现模块属于内部接口。应用从 :doc:`api` 开始，不应在同一连接并行创建多个所有者/解析器。以下签名使用 JavaScript 参数/默认值语法，异步方法返回 Promise。

@esp-vision/device-core
----------------------------------------------

根入口仅导出 ``createDevice(options)``，全部参数与返回设备对象的方法见 :doc:`api`。

@esp-vision/device-core/evmux
----------------------------------------------------------

EvMuxParser
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EvMuxParser(handlers = {})``

增量字节解析器，处理不完整帧边界、CRC、过滤和 channel 分发。handlers 包括 text、rawText、log、command、other、preview(base64, metadata, bytes)、debugImage(base64, metadata)、rpc(metadata, payload)、filterFrame(metadata, payload)、frameRejected(metadata)。filterFrame 返回 false 时阻止该帧交付。

``EvMuxParser.setLimits({ metadataMax, payloadMax, frameProgressTimeoutMs, frameTotalTimeoutMs } = {})``
    更新正值 metadata/payload 大小和不完整帧时间限制。

``EvMuxParser.resetReplTextDecoders()``
    重置 stdout/stderr 的 UTF-8 解码状态。

``EvMuxParser.reset()``
    清除输入缓冲、不完整帧定时器与统计。

``EvMuxParser.getDiagnostics()``
    返回 active/rxBytes/rxChunks/bufferedBytes/frames/droppedFrames/lastChannel。

``EvMuxParser.hasPartialFrame()``
    判断缓冲中是否存在可能的不完整帧。

``EvMuxParser.feedText(text)``
    将兼容文本编码后输入解析器，优先使用字节入口。

``EvMuxParser.feedBytes(bytes)``
    输入任意 Uint8Array 块，允许拆帧、粘帧。

``EvMuxParser.dispatch(metadata, payload)``
    将已解析 metadata/payload 分发至 channel handler，此入口绕过线上帧校验。

makeEvMuxFrame
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``makeEvMuxFrame({ channel, type = 'data', method = null, payload = '', seq = 0, sid = 'web-ide', sessionId = undefined, vmEpoch = undefined, contentType = undefined, width = undefined, height = undefined, route = undefined, encoding = undefined })``

返回含 metadata、文本/二进制 payload 和 CRC 的线上 Uint8Array。seq 用于关联，sessionId/vmEpoch 标识所有权代次；编码不发送数据，也不获取会话。

@esp-vision/device-core/evrepl
------------------------------------------------------------

EV_REPL_FILE_CHUNK_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

兼容 REPL 文件写入器的默认块大小，与 script.write RPC 分块不同。

.. code-block:: json

   224

EV_REPL_INPUT_MAX_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

完整 REPL 输入命令的默认 UTF-8 预算，包括 exec 转义开销。

.. code-block:: json

   511

EvRepl
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EvRepl(io)``

framed MicroPython REPL 协调器。io.writeEvmux(channel, type, method, payload) 必须返回 Promise，并持续输入 stdout。下列公开方法也包含解释器记账辅助操作，应用通常使用 device.python/files。

``EvRepl.execInputBytes(source)``
    计算转义后的 REPL exec 命令 UTF-8 字节数，包含命令包装开销。

``EvRepl.createScriptEndSentinel()``
    生成并返回唯一脚本完成标记打印命令。

``EvRepl.createScriptRunCommand(path)``
    生成含内联完成标记的 execfile 运行命令。

``EvRepl.cancelScriptEndSentinel()``
    清除待匹配的完成标记。

``EvRepl.drainScriptEnd()``
    消费缓冲的脚本完成标记，通知 onScriptEnd。

``EvRepl.partialMarkerTail()``
    查找末尾不完整完成标记以保留缓冲。

``EvRepl.feedStdout(text)``
    输入文字，推进提示符/输出等待者。

``EvRepl.drain()``
    处理已缓冲的提示符与完成数据。

``EvRepl.failAll(error)``
    以错误拒绝排队/当前等待者。

``EvRepl.resetEpoch(error)``
    重置解释器 epoch 状态，使旧等待者失效。

``EvRepl.enqueue(operation, { signal = null } = {})``
    串行执行 operation，返回其结果 Promise，支持取消。

``EvRepl.writeStdin(data)``
    向 repl.stdin 写字符串/Uint8Array。

``EvRepl.execLine(line, { timeout = 8000, signal = null } = {})``
    执行准备好的行，等待提示符/输出。

``EvRepl.execPy(source, { timeout = 8000, signal = null } = {})``
    将源码转义为 exec 命令，返回收集的输出。

``EvRepl.getDeviceInfo({ signal = null } = {})``
    查询平台、实现、板卡及解释器信息。

``EvRepl.getFsStats(path = '/', { signal = null } = {})``
    查询路径所在文件系统容量/空闲信息。

``EvRepl.listFs(path, { recursive = false, signal = null } = {})``
    返回平铺 {name,path,type,size} 条目，可递归。

``EvRepl.walkFs({ signal = null } = {})``
    返回兼容接口使用的嵌套文件树。

``EvRepl.readFile(fn, { timeout = null, chunkTimeout = 30000, signal = null, maxBytes = null } = {})``
    返回完整文件 Uint8Array；文件变化、超限或不完整时拒绝。

``EvRepl.touchFile(fn)``
    创建/截断一个文件，Promise<void>。

``EvRepl.removeFile(path, { signal = null } = {})``
    删除一个文件，Promise<void>。

``EvRepl.removeDir(path, { signal = null } = {})``
    删除空目录，Promise<void>。

``EvRepl.makePath(path)``
    通过兼容 REPL 辅助方法创建路径。

``EvRepl.writeFile(fn, data, chunk_size = EV_REPL_FILE_CHUNK_BYTES, direct = false, { signal = null } = {})``
    兼容 REPL 字节/分块写入器；应用 files.write 使用会话 RPC 写入器。

``EvRepl.readSysInfoMD()``
    返回 Markdown 格式的系统信息。

evReplExecInputBytes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``evReplExecInputBytes(source)``

计算转义后的 REPL exec 命令 UTF-8 字节数，包含命令包装开销。

@esp-vision/device-core/session
--------------------------------------------------------------

DeviceSession
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceSession({ transport, EvMuxParser, makeEvMuxFrame, EvRepl, handlers = {}, sid = 'ide-client', sleep = defaultSleep, })``

底层 v4 解释器，注入 transport、EvMuxParser、EvRepl、makeEvMuxFrame；handlers 接收 text/rawText/log/preview/rpc/diagnostic 及会话生命周期回调。调用方须订阅传输输入并喂入字节，构造类本身不打开连接；应用入口 createDevice 已封装这些生命周期操作。

``DeviceSession.feedBytes(bytes)``
    向会话解析器输入接收字节。

``DeviceSession.feedText(text)``
    输入兼容文本。

``DeviceSession.hasFeature(feature)``
    判断协商能力是否声明指定 feature。

``DeviceSession.writeFrame(channel, type, method, payload, { authorize = null, vmRequest = null, seq: requestedSeq = null, signal = null, writeTimeout = null, } = {})``
    按当前会话/epoch 编码发送帧，返回传输 Promise。

``DeviceSession.rpc(method, payload = {}, { timeout = 4000, channel = null, signal = null, binaryResponse = false } = {})``
    发送关联 RPC，等待 JSON 或 binaryResponse；超时、取消、固件错误会拒绝。

``DeviceSession.captureImage(method, payload = {}, options = {})``
    调用抓图方法，返回 JPEG {data,mimeType,metadata}。

``DeviceSession.negotiate(options = {})``
    探测 v4、校验身份/限制、获取所有权，可验证 REPL。

``DeviceSession.writeFile(path, content, { encoding = 'utf-8', mode = 'overwrite', runAfterWrite = false, signal = null, onProgress = null, } = {})``
    通过统一的 file.write 二进制引擎写入 UTF-8/字节，完成 SHA-256 提交后返回；要求 file.write.binary，不回退旧上传路径。

``DeviceSession.writeScript(path, content, { runAfterWrite = false, signal = null } = {})``
    writeFile 的 UTF-8 兼容封装。

``DeviceSession.createOperationId()``
    生成新的网络修改 operation ID。

``DeviceSession.networkStatus(options = {})``
    返回 network.status 状态/结果。

``DeviceSession.configureNetwork({ ssid, password, operationId = undefined }, options = {})``
    发送凭据和 operationId，返回 accepted ACK，不代表最终联网成功。

``DeviceSession.setNetworkEnabled(enabled, options = {})``
    发送持久化网络开关修改。

``DeviceSession.forgetNetwork(options = {})``
    请求清除持久化凭据。

``DeviceSession.release({ timeout = 1200 } = {})``
    释放 EV-MUX 会话租约，Promise 返回 RPC 结果。

``DeviceSession.executePython(source, { timeout = null, signal = null } = {})``
    通过 framed REPL 执行源码并返回输出。

``DeviceSession.interrupt()``
    通过 framed signal 通道发送 Ctrl-C。

``DeviceSession.control(operation, { timeout = 15000, signal = null } = {})``
    执行 stop/soft-reset/hard-reset/bootloader 控制。

``DeviceSession.stopForRepl({ timeout = 5000, signal = null } = {})``
    中断并等待可执行 REPL，返回就绪布尔值。

``DeviceSession.runScript(path, content, { timeout = 10000, signal = null, onStarted = null, maxOutputChars = Number.POSITIVE_INFINITY, trial = null, inlineCompletion = false, } = {})``
    执行设备脚本，可先上传 content，等待有界完成。

``DeviceSession.startScript(path, content, { onStarted = null, ...options } = {})``
    启动脚本，返回含 done Promise 的句柄。

``DeviceSession.scriptRunSnapshot()``
    返回当前运行快照或 null。

``DeviceSession.snapshot()``
    返回会话状态、role、能力、parser/transport 统计和待处理 RPC 数量。

``DeviceSession.close(error = new Error('connection closed'))``
    拒绝待处理工作，重置会话状态/定时器；物理连接由宿主关闭。

可读取的协商状态包括 state、role、identity、capabilities、sessionId、vmEpoch、vmReady、limits、parser、repl；应通过会话方法更新，不直接赋值。``negotiate`` 选项为 role="unknown"、drainMs=300、acquire=true、probeRepl=true、expectedDevice=null、signal=null，TCP 跳过启动 drain。``control`` 接受 stop/soft-reset/hard-reset/bootloader 字符串，不是应用对象的请求结构。底层网络修改返回 accepted ACK，应用 configure 才会等到对应事务完成。使用 typed run trial 时要求 operation_id 为 UUIDv4、source_path 不超过 512 个 UTF-8 字节、source_hash 为 64 位小写 SHA-256，并要求固件声明 launcher trial/typed-runner 能力；trial 不能与 inlineCompletion 组合。

EV_MUX_DEFAULT_LIMITS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

协议回退限制，实际操作使用协商的固件 limits；txLargePayloadMax=0 是初始回退值，不表示所有设备只能发送零字节图像。

.. code-block:: json

   {
     "metadataMax": 384,
     "rxPayloadMax": 2048,
     "replInputMax": 511,
     "vmPendingMax": 1,
     "txLargePayloadMax": 0,
     "txFrameTimeoutMs": 1500,
     "txQueueTimeoutMs": 2000
   }

EV_MUX_PATH_MAX_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

设备路径 UTF-8 字节限制，已考虑临时文件名余量。

.. code-block:: json

   123

EV_MUX_PAYLOAD_LIMIT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

兼容 payload 预算常量，不替代协商的 RX 限制。

.. code-block:: json

   1800

EV_MUX_REQUIRED_CHANNELS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

v4 能力校验要求的 channel 集合。

.. code-block:: json

   [
     "user.rpc",
     "debug.rpc",
     "repl.stdin",
     "repl.stdout",
     "repl.stderr",
     "repl.signal",
     "log.idf",
     "preview.frame"
   ]

EV_MUX_REQUIRED_FEATURES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

v4 能力校验要求的 feature 集合。

.. code-block:: json

   [
     "capabilities",
     "route.changed",
     "framed.repl",
     "session.firstCome",
     "session.keepalive"
   ]

EV_MUX_SCRIPT_CHUNK_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

旧 script.write 的参考常量；SDK 0.3.0 上传不再使用，二进制块长取自 fileWriteChunkMax。

.. code-block:: json

   900

EV_MUX_VERSION
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

支持的会话协议版本。

.. code-block:: json

   4

evmuxRpcChannelForMethod
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``evmuxRpcChannelForMethod(method)``

debug.* 方法返回 debug.rpc，其他返回 user.rpc。

requiresEvmuxFirmwareUpgrade
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``requiresEvmuxFirmwareUpgrade(err)``

判断是否为需要升级固件的错误，返回布尔值，包括版本不匹配或固件升级标记。

validateEvmuxCapabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``validateEvmuxCapabilities(payload)``

校验 v4 版本、必需 features/channels，并校验会话租约，返回 void；不匹配或缺失能力时抛错。

@esp-vision/device-core/actions
--------------------------------------------------------------

DEVICE_ACTION_DESCRIPTORS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

下列 17 个描述定义宿主动作契约，不代表 Core 已实现平台端口枚举、连接 UI 或 workspace 部署；这些 handler 须显式绑定。输入以各描述的 inputSchema 为准，outputSchema 允许 handler 自定义附加字段，risk 含 readOnly/destructive/openWorld 标志。

``device.listPorts``

List platform-native serial candidates for the user to select; this action never chooses a device.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": true,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": false,
     "concurrency": "shared-read"
   }

``device.connect``

Connect to an ESP Vision device using standard elicitation for a serial port or an explicit device TCP address, then acquire an EV-MUX v4 session.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "transport": {
           "enum": [
             "serial",
             "tcp"
           ],
           "default": "serial",
           "description": "Use serial for a platform port or tcp for an IP address on the device network."
         },
         "port": {
           "type": "string",
           "description": "Platform-native serial identifier, such as /dev/ttyACM0 or /dev/cu.usbmodem*. Omit it only when the MCP host advertises elicitation or ESP_VISION_PORT is configured. Non-interactive callers must pass it explicitly. A direct network address may also be supplied as tcp://IP:PORT or IP:PORT; alternatively set transport to tcp and provide host and tcpPort."
         },
         "host": {
           "type": "string",
           "description": "IPv4 or IPv6 address of the device when transport is tcp."
         },
         "tcpPort": {
           "type": "integer",
           "minimum": 1,
           "maximum": 65535,
           "default": 3232,
           "description": "Device EV-MUX TCP port."
         },
         "baudRate": {
           "type": "integer",
           "minimum": 1200,
           "maximum": 4000000,
           "default": 115200
         },
         "drainMs": {
           "type": "integer",
           "minimum": 0,
           "maximum": 3000,
           "default": 300
         }
       },
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": false,
       "openWorld": true
     },
     "requiresReadyDevice": false,
     "concurrency": "exclusive"
   }

``device.disconnect``

Disconnect the active device session and release its lease.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": false
     },
     "requiresReadyDevice": false,
     "concurrency": "exclusive"
   }

``device.negotiate``

Discover EV-MUX v4, explicitly acquire a session, and prove the framed REPL is live.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "role": {
           "type": "string",
           "default": "unknown"
         },
         "drainMs": {
           "type": "integer",
           "minimum": 0,
           "maximum": 3000,
           "default": 300
         }
       },
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": false,
     "concurrency": "exclusive"
   }

``device.inspect``

Inspect protocol state, device identity, filesystem, and recent logs.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "includeFiles": {
           "type": "boolean",
           "default": true
         },
         "logLimit": {
           "type": "integer",
           "minimum": 0,
           "maximum": 500,
           "default": 100
         }
       },
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": true,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "shared-read"
   }

``python.execute``

Execute MicroPython through the shared interpreter using bounded REPL or binary file staging.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "code": {
           "type": "string"
         },
         "timeoutMs": {
           "type": "integer",
           "minimum": 100,
           "maximum": 120000,
           "default": 8000
         }
       },
       "required": [
         "code"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``python.runEphemeral``

Run one Python draft from the reserved temporary device path without modifying a user-owned file.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "code": {
           "type": "string"
         },
         "maxDurationMs": {
           "type": "integer",
           "minimum": 1000,
           "maximum": 180000,
           "default": 180000
         }
       },
       "required": [
         "code"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``python.observeEphemeral``

Observe the bounded output and status of the current temporary Python run.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "runId": {
           "type": "string"
         },
         "waitMs": {
           "type": "integer",
           "minimum": 0,
           "maximum": 10000,
           "default": 1000
         },
         "outputOffset": {
           "type": "integer",
           "minimum": 0,
           "maximum": 1048576,
           "default": 0
         }
       },
       "required": [
         "runId"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": true,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "shared-read"
   }

``python.stopEphemeral``

Stop the current temporary Python run and remove its reserved device file.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "runId": {
           "type": "string"
         }
       },
       "required": [
         "runId"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``deviceFile.read``

Read a file from the device filesystem.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "path": {
           "type": "string"
         },
         "encoding": {
           "enum": [
             "utf-8",
             "base64"
           ],
           "default": "utf-8"
         }
       },
       "required": [
         "path"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": true,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``deviceFile.write``

Write a file to the device using the negotiated transfer path.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "path": {
           "type": "string"
         },
         "content": {
           "type": "string"
         },
         "encoding": {
           "enum": [
             "utf-8",
             "base64"
           ],
           "default": "utf-8"
         },
         "runAfterWrite": {
           "type": "boolean",
           "default": false
         }
       },
       "required": [
         "path",
         "content"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``workspaceFile.deploy``

Stream one workspace-relative file to the device without placing binary content in the model context.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "sourcePath": {
           "type": "string",
           "description": "Workspace-relative source file path."
         },
         "devicePath": {
           "type": "string",
           "description": "Absolute destination path on the device."
         },
         "expectedBytes": {
           "type": "integer",
           "minimum": 0,
           "maximum": 16777216
         },
         "expectedSha256": {
           "type": "string",
           "description": "Optional lowercase SHA-256 expected before transfer."
         },
         "overwrite": {
           "type": "boolean",
           "default": false
         }
       },
       "required": [
         "sourcePath",
         "devicePath"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``deviceFile.remove``

Remove one file or one empty directory from the device filesystem.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "path": {
           "type": "string"
         },
         "kind": {
           "enum": [
             "file",
             "directory"
           ],
           "default": "file"
         }
       },
       "required": [
         "path"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``deviceTest.run``

Write, run, and capture a target-side MicroPython test.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "path": {
           "type": "string",
           "default": "/.agent_test.py"
         },
         "source": {
           "type": "string"
         },
         "timeoutMs": {
           "type": "integer",
           "minimum": 500,
           "maximum": 120000,
           "default": 10000
         }
       },
       "required": [
         "source"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``device.control``

Interrupt, reset, or enter the bootloader.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "operation": {
           "enum": [
             "stop",
             "soft-reset",
             "hard-reset",
             "bootloader"
           ]
         },
         "confirmBootloader": {
           "type": "boolean",
           "default": false
         }
       },
       "required": [
         "operation"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``device.rpc``

Call a negotiated user or debug RPC method. For network.configure, network.enable, and network.forget, include a 32-character lowercase hexadecimal operationId and reuse the exact same ID and payload when retrying after a timeout.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "method": {
           "type": "string"
         },
         "payload": {
           "type": "object",
           "default": {}
         },
         "channel": {
           "enum": [
             "user.rpc",
             "debug.rpc"
           ]
         },
         "timeoutMs": {
           "type": "integer",
           "minimum": 100,
           "maximum": 120000,
           "default": 4000
         }
       },
       "required": [
         "method"
       ],
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": false,
       "destructive": true,
       "openWorld": true
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

``frame.capture``

Capture one JPEG frame for an explicitly requested observation.

.. code-block:: json

   {
     "inputSchema": {
       "type": "object",
       "properties": {
         "timeoutMs": {
           "type": "integer",
           "minimum": 500,
           "maximum": 30000,
           "default": 8000
         }
       },
       "additionalProperties": false
     },
     "outputSchema": {
       "type": "object",
       "properties": {},
       "additionalProperties": true
     },
     "risk": {
       "readOnly": true,
       "destructive": false,
       "openWorld": false
     },
     "requiresReadyDevice": true,
     "concurrency": "exclusive"
   }

DEVICE_ACTION_SCHEMA_VERSION
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

宿主动作描述的 schema 版本。

.. code-block:: json

   1

DEVICE_ACTORS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

宿主动作层允许的 actor 名称，普通设备对象不要求 actor。

.. code-block:: json

   [
     "agent"
   ]

DeviceActionError
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionError(code, message, { recovery = null, details = null, cause = null } = {})``

结构化宿主动作错误，含 code/message 和可选 recovery/details/cause；toJSON 不序列化 cause。

``DeviceActionError.toJSON()``
    返回 {code,message,recovery?,details?}。

DeviceActionGateway
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionGateway({ registry = new DeviceActionRegistry(), coordinator = new OperationCoordinator(), lease = new DeviceLease(), getSession = () => null, authorize = async () => true, } = {})``

校验动作输入/context，检查宿主租约与 ready 会话，调用 authorize(descriptor, input, context)，再协调已绑定 handler。context.actor 必须受支持，context.requestId 必填，context.signal 支持取消。宿主须提供实际授权策略，默认 authorize 返回 true。

``DeviceActionGateway.invoke(actionName, input = {}, context = {})``
    Promise：返回校验/授权后 handler 的结果；错误附动作上下文。

DeviceActionRegistry
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionRegistry(descriptors = DEVICE_ACTION_DESCRIPTORS, { schemaVersion = DEVICE_ACTION_SCHEMA_VERSION } = {})``

动作描述与显式绑定 handler 的注册表。描述要求 name、inputSchema、outputSchema、risk。未绑定描述仅是元数据，Gateway.invoke 会以 ACTION_UNAVAILABLE 拒绝调用；register/bind 返回 this，可链式调用。

``DeviceActionRegistry.register(descriptor, handler = null)``
    注册描述与可选 handler。

``DeviceActionRegistry.bind(name, handler)``
    为已有动作绑定 handler。

``DeviceActionRegistry.descriptor(name)``
    返回描述或 null。

``DeviceActionRegistry.handler(name)``
    返回 handler 或 null。

``DeviceActionRegistry.list()``
    返回描述数组。

``DeviceActionRegistry.validate(name, input)``
    校验输入、应用默认值，返回校验后的副本。

DeviceLease
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceLease()``

宿主侧 actor 租约，与固件 EV-MUX 会话租约不同。actor 必须属于 DEVICE_ACTORS，同 actor 的多个句柄按数量维护；它不获取或强制抢占设备会话。

``DeviceLease.acquire(actor)``
    返回 {id,actor,release}，不同 actor 冲突时抛错。

``DeviceLease.release(id)``
    释放一个宿主租约 ID。

``DeviceLease.assertAccess(actor)``
    actor 未持有宿主租约时抛错。

``DeviceLease.snapshot()``
    返回 {state,primary,actors,holders}。

DeviceLeaseConflictError
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceLeaseConflictError(owner, requester)``

宿主 actor 与当前租约持有者冲突时的 DeviceActionError。

OperationCoordinator
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new OperationCoordinator()``

支持取消与关闭处理的宿主串行队列。此适配层会排队，设备对象自身则拒绝冲突；operation 接收对应 AbortSignal。

``OperationCoordinator.run({ actor, requestId, actionName, signal }, operation)``
    将 operation 排队，返回结果 Promise。

``OperationCoordinator.close(reason = actionAbortError('Device session closed'))``
    拒绝排队工作并取消当前工作，停止接受新操作。

``OperationCoordinator.snapshot()``
    返回当前及排队的 actor/request/action 身份。

@esp-vision/device-core/bindings
----------------------------------------------------------------

DeviceEvidenceBuffer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceEvidenceBuffer(limit = MAX_EVIDENCE_ENTRIES)``

有界宿主证据历史，构造默认上限 500，snapshot 默认 100；值会限长，应用仍须处理敏感信息。

``DeviceEvidenceBuffer.append(kind, value, details = null)``
    追加带时间戳、限长的值及可选 details。

``DeviceEvidenceBuffer.snapshot(limit = 100)``
    返回最近若干条记录。

``DeviceEvidenceBuffer.clear()``
    清空历史。

EPHEMERAL_PYTHON_PATH
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

高级临时运行控制器的保留路径；应用 source/start 使用自己的唯一临时路径。

.. code-block:: json

   "/.espvision_agent_run.py"

EphemeralPythonController
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EphemeralPythonController(session, { evidence = null, generation = 'session' } = {})``

每个控制器管理一个有时限的临时 Python 运行，使用 EPHEMERAL_PYTHON_PATH；session 为 DeviceSession。快照含 runId/path/status/output/nextOffset/finished/cleaned，可附 error/outputTruncated/cleanupError/residualPath；启动成功不代表清理完成。

``EphemeralPythonController.start(code, { maxDurationMs = 180000, signal = null } = {})``
    启动有时限的临时源码运行，返回快照，完成后清理。

``EphemeralPythonController.observe(runId, { waitMs = 1000, outputOffset = 0, signal = null } = {})``
    最多等待 waitMs，从 outputOffset 返回增量输出。

``EphemeralPythonController.stop(runId)``
    停止指定运行，返回清理/状态快照。

``EphemeralPythonController.close()``
    控制器关闭时停止/清理当前运行。

bindSessionDeviceActions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``bindSessionDeviceActions(registry, { evidence = new DeviceEvidenceBuffer(), lease = null, getOperations = () => null, getGeneration = () => 'session', } = {})``

向注册表绑定通用会话/文件/Python/抓图 handler，返回 { evidence, controllerFor }。此函数不提供宿主专用的 listPorts/connect/disconnect/workspace 部署实现，须由宿主绑定。

@esp-vision/device-core/firmware
----------------------------------------------------------------

FirmwareFlashSession
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new FirmwareFlashSession(loader, image, { reset = () => loader.hardReset(), mode = 'update' } = {})``

ROM 烧写器适配，与 EV-MUX 运行态会话分开，每个实例只使用一次。state 为 idle → connecting → connected → flashing → verified → closed，失败可经 error → closed；flash 最终会断开，单凭 closed 不能判定校验成功。

``FirmwareFlashSession.connect(bytes)``
    写入前校验镜像、芯片/版本/安全模式和现有分区表哈希。

``FirmwareFlashSession.flash(bytes, { onProgress = () => {}, onPhase = () => {} } = {})``
    写应用分区，完整读回并核验 SHA-256，然后复位；最终断开。onProgress 为 0..100，onPhase 接收状态文字。

``FirmwareFlashSession.disconnect()``
    关闭 loader 一次，重复调用不重复关闭。

镜像描述包含 evMuxVersion=4、bytes、offset、partitionSize、sha256、chipFamily、chipRevision、partitionTable:{offset,bytes,sha256}。须提供可信构建元数据与精确 Uint8Array；默认模式不执行整片擦除或分区迁移。注入的 loader 提供 initialize()、chipFamily/chipRevision、getSecurityInfo()、runStub(true)、hardReset()、disconnect()；stub 提供 flashData()、readFlash()、readRegister()、writeRegister()。内置 P4 正常启动锁存处理。读回校验通过后的复位失败由 onPhase 提示，不会撤销校验或自动重写。


``mode: 'install'`` 使用完整合并镜像描述：原应用元数据放在 application 中，bytes/offset=0/sha256/flashSize/bootloader 描述完整镜像，partitionTable 保持原结构；芯片修订范围须同时覆盖 bootloader 与应用。安装跳过旧分区表匹配，要求返回的 stub 提供 flashId()，返回整数 JEDEC ID，位 16–23 为容量指数。现有应用更新适配器无需修改；内置 Node 适配器及官网 Web Serial loader 提供该方法。完整安装的数据覆盖语义见 :doc:`api`。

``firmwareMode(mode = 'update')`` 校验并返回 update/install，其他值抛出 TypeError。``validateFirmwareImage(image, bytes, { mode = 'update' } = {})`` 在 install 模式下还会校验嵌入的 bootloader、分区表和应用范围及哈希。

firmwareSha256
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``firmwareSha256(bytes)``

Promise<string>，返回小写 SHA-256 十六进制摘要，要求 Web Crypto。

validateFirmwareImage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``validateFirmwareImage(image, bytes, { mode = 'update' } = {})``

Promise<Uint8Array>，校验 v4、精确字节数、应用分区边界和 SHA-256，返回输入字节；不推断或下载可信构建清单。

@esp-vision/device-core/node
------------------------------------------------------------------

``defaultPython``
    平台默认 Python 可执行文件：Windows 为 ``python``，其他系统为 ``python3``。

``listSerial({ python = defaultPython, signal, timeout = 10000 } = {}, dependencies = {})``
    使用 pyserial 枚举系统串口。需要 Python 与 pyserial；可选依赖对象允许注入进程执行器。

``filterSerialPorts(ports, { platform = process.platform, allPorts = false } = {})``
    按平台路径及 USB 描述符提示筛选系统枚举记录，不打开端口，也不认定固件身份。

``serialEntry(port)``
    将串口描述符转换为未探测的接口/地址候选并附带 USB 元数据；描述符分类不等于设备身份。

``transportFor(entry, options = {})``
    为枚举项创建被动 TCP 或串口字节传输；串口选项可指定 ``python``。

``inventory(options = {}, dependencies = {})``
    从串口枚举和局域网发现返回 ``{ devices, warnings, sourcesAvailable }``。默认只扫描，不连接；``probe: true`` 会连接每个候选。可用 ``timeout``、``interfaces``、``python``、``allPorts`` 和 ``signal`` 控制扫描。

``nodeAdapterAssets()``
    返回包含 ``serial`` 和 ``rom`` URL 的冻结对象，分别指向 ``node_serial.py`` 与 ``node_rom.py``。Node 打包器须将两个资源复制到已约定的打包入口旁路径。

``tcpTransport({ host, port })``
    返回被动构造的 Node TCP 字节适配器。由 Core 在 ``device.connect({ role: 'tcp' })`` 时打开。

``serialTransport({ serial, python })``
    返回共用的 Node/Python 串口字节适配器。指定设备名称/路径及已安装 pyserial 的解释器（例如 ``python3``）；由 Core 打开和关闭，按实际接口选择 ``usj``、``usb`` 或 ``uart`` 角色。适配器只传字节，不维护另一套设备会话。


``openNodeRom({ port, python?, signal? })``
    返回内置 ROM 适配器，创建时不打开端口。用于 ``device.firmware.update({ openLoader: ({ signal }) => openNodeRom({ port, signal }), ... })``，安装与应用更新共用。需要独立安装 Python esptool>=5.3,<6；Unix 默认 python3，Windows 默认 python。Core 负责初始化、校验、写入/回读、复位与释放。SDK 0.3.0 新增，仅用于 Node；区别于运行态 serialTransport，也不是 TCP OTA。

@esp-vision/device-core/discovery
------------------------------------------------------------------

ESP_VISION_DISCOVERY_SERVICE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Node provider 查询的 DNS-SD 服务名。

.. code-block:: json

   "_esp-vision._tcp.local"

NetworkDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new NetworkDiscovery(provider)``

包装含 list({timeout, signal}) 的 provider，校验结果并按主机名排序；list 与 device.discover 使用相同限制及结果结构，构造时不扫描。

``NetworkDiscovery.list({ timeout = 3000, signal } = {})``
    在发现时间窗口内收集规范记录，支持 AbortSignal。

createBridgeDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``createBridgeDiscovery(connectionUrl, { fetch: fetcher = globalThis.fetch } = {})``

返回基于桥接 GET /devices?timeout=... 的 NetworkDiscovery；接收 ws/wss connectionUrl，可注入 fetch。结果中的 connectionUrl 须同源且为 /device/<32-hex> 路径。

normalizeDiscoveredDevice
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``normalizeDiscoveredDevice(value)``

返回规范记录或抛错：12 位小写 MAC 十六进制、DNS 主机名不超过 63 字符、board 不超过 96、firmwareVersion 不超过 255、整数 evMuxVersion、端口 1..65535、至少一个合法 IPv4；地址去重，compatible 表示 v4。

@esp-vision/device-core/discovery/node
----------------------------------------------------------------------------

createNodeDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``createNodeDiscovery({ createMdns, interfaceAddress } = {})``

返回使用 _esp-vision._tcp.local 的 PTR/SRV/TXT/A 记录的 NetworkDiscovery。createMdns 可注入兼容 multicast-dns 的工厂，interfaceAddress 选择网卡 IPv4；否则动态导入可选依赖 multicast-dns。仅接受私网/链路本地 IPv4 候选，遵守 TTL，每轮扫描后关闭 socket。

使用边界
----------------------------------------

子路径导出是实际实现或宿主契约，不是所有应用请求的别名。同一物理设备保持一个会话协调入口；设备所有权、宿主 actor 租约、ROM 烧写生命周期和发现扫描含义不同。普通设备交互使用应用对象，仅在实现对应宿主层时选用子路径。

FirmwareFlashSession.flash 返回值
------------------------------------------

回读校验后，flash 返回 ``{ verified: true, needsReset: boolean }``。复位失败时 needsReset 为 true；仅凭 closed 状态不能证明校验成功。应用通常使用 ``device.firmware.update()`` 完成发布目录校验与端口交接。


@esp-vision/device-core/browser
------------------------------------------------------------------

``openBrowserRom({ port, logger = {}, signal? })``
    SDK 0.5.1 已包含此入口。为用户选择的 Web Serial ``SerialPort`` 返回被动 ROM 适配器；应在用户手势内申请端口，先释放已有运行时连接。``initialize()`` 延迟加载锁定的 tasmota 9.4.2，打开端口、识别芯片并提供 ``chipName/chipFamily/chipRevision``，不启动 stub。适配器实现 ``initialize/getSecurityInfo/runStub/hardReset/disconnect`` 供 ``firmware.update`` 使用；Core 校验镜像与 ROM 安全模式后才启动 stub，统一负责写入、完整回读和复位。``logger`` 可提供 ``log/error/debug``；``signal`` 取消初始化，不中断已开始的写入与回读。``disconnect()`` 幂等释放端口，初始化失败也清理资源；关闭后不可复用。须使用浏览器打包器，不支持直接加载上游原生 ESM。完整示例见 :doc:`integration`。此入口不加载 Node 模块，根与 Node 入口也不加载浏览器后端。

@esp-vision/device-core/firmware 发布读取器（0.5.1）
------------------------------------------------------

``loadOfficialFirmware({ board?, version?, fetch = globalThis.fetch, signal? })``
    通过 publication 引用加载官方当前或精确指定版本的快照，返回 manifest 及 publication 历史；可选板型筛选前校验整份快照。无 publication 的配置保留旧元数据兼容。

``loadReleaseManifest({ url, sha256, version?, sourceCommit?, boards?, fetch = globalThis.fetch, signal? })``
    只加载一份不可变官方 manifest，校验原始字节 SHA-256、身份、完整板集、所有固件描述及 website/model/MCP 来源边界，返回 manifest 对象，不下载固件也不打开 ROM。字段、限制与单 BIN 升级行为见 :doc:`api`。

浏览器烧录后的启动
---------------------

浏览器 ROM 适配器在校验后通过当前运行的 flash stub 复位。写入校验成功不代表固件已经启动：宿主必须等待新的运行态握手后才允许配网。若 ``needsReset`` 为真，``resetError`` 会保留复位失败的代码和消息；手动复位只作为异常恢复方式。
