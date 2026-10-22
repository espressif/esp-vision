Advanced export API reference
==========================================================

:link_to_translation:`zh_CN:[中文]`

This reference follows every entry of package.json exports: the public import paths and named exports. It lists every non-underscore public prototype method of the exported classes. Underscore methods and src/device_* implementation modules without a package export are internal. Application developers should start with :doc:`api`, not combine multiple owners/parsers on one connection. Signatures below use JavaScript parameter/default syntax; asynchronous methods return Promises.

@esp-vision/device-core
----------------------------------------------

Only ``createDevice(options)`` is exported at the root; all parameters and the returned device API are specified in :doc:`api`.

@esp-vision/device-core/evmux
----------------------------------------------------------

EvMuxParser
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EvMuxParser(handlers = {})``

Incremental byte parser with bounded partial frames, CRC checks, frame filtering and channel dispatch. handlers: text, rawText, log, command, other, preview(base64, metadata, bytes), debugImage(base64, metadata), rpc(metadata, payload), filterFrame(metadata, payload), frameRejected(metadata). A filter returning false rejects a frame before delivery.

``EvMuxParser.setLimits({ metadataMax, payloadMax, frameProgressTimeoutMs, frameTotalTimeoutMs } = {})``
    Update positive metadata/payload sizes and partial-frame timing limits.

``EvMuxParser.resetReplTextDecoders()``
    Reset stdout/stderr UTF-8 decoder state.

``EvMuxParser.reset()``
    Clear buffered data, partial-frame timers and diagnostics.

``EvMuxParser.getDiagnostics()``
    Return active/rxBytes/rxChunks/bufferedBytes/frames/droppedFrames/lastChannel.

``EvMuxParser.hasPartialFrame()``
    Return whether buffered bytes form an incomplete possible frame.

``EvMuxParser.feedText(text)``
    Encode legacy text input and feed the parser; byte input is preferred.

``EvMuxParser.feedBytes(bytes)``
    Feed arbitrary Uint8Array chunks, including split/coalesced frames.

``EvMuxParser.dispatch(metadata, payload)``
    Dispatch already parsed metadata/payload to channel handlers; this bypasses wire validation.

makeEvMuxFrame
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``makeEvMuxFrame({ channel, type = 'data', method = null, payload = '', seq = 0, sid = 'web-ide', sessionId = undefined, vmEpoch = undefined, contentType = undefined, width = undefined, height = undefined, route = undefined, encoding = undefined })``

Return Uint8Array wire bytes including metadata, binary/text payload and CRC. seq is correlation, sessionId/vmEpoch are ownership generations; encoding a frame does not send it or acquire a session.

@esp-vision/device-core/evrepl
------------------------------------------------------------

EV_REPL_FILE_CHUNK_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Default chunk size for the legacy REPL file writer, separate from script.write RPC chunks.

.. code-block:: json

   224

EV_REPL_INPUT_MAX_BYTES
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Default UTF-8 budget for a complete REPL input command including exec escaping.

.. code-block:: json

   511

EvRepl
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EvRepl(io)``

Framed MicroPython REPL coordinator. io.writeEvmux(channel, type, method, payload) must return a Promise. stdout must be fed continuously. Public helper methods below include interpreter bookkeeping; application callers normally use device.python/files instead.

``EvRepl.execInputBytes(source)``
    Return the UTF-8 byte count of the escaped REPL exec command, including command overhead.

``EvRepl.createScriptEndSentinel()``
    Create and return a unique script-completion print command.

``EvRepl.createScriptRunCommand(path)``
    Create an execfile runner with an inline completion sentinel.

``EvRepl.cancelScriptEndSentinel()``
    Discard the outstanding completion marker.

``EvRepl.drainScriptEnd()``
    Consume buffered script-completion markers and notify onScriptEnd.

``EvRepl.partialMarkerTail()``
    Find a trailing partial completion marker for buffering.

``EvRepl.feedStdout(text)``
    Feed text and resolve prompt/output waiters.

``EvRepl.drain()``
    Process buffered prompt and completion data.

``EvRepl.failAll(error)``
    Reject queued/current waiters with an error.

``EvRepl.resetEpoch(error)``
    Reset interpreter epoch state and invalidate old waiters.

``EvRepl.enqueue(operation, { signal = null } = {})``
    Serialize operation; returns its result Promise and supports cancellation.

``EvRepl.writeStdin(data)``
    Write string/Uint8Array to repl.stdin.

``EvRepl.execLine(line, { timeout = 8000, signal = null } = {})``
    Execute a prepared line and await its prompt/output.

``EvRepl.execPy(source, { timeout = 8000, signal = null } = {})``
    Escape source into an exec command and return collected output.

``EvRepl.getDeviceInfo({ signal = null } = {})``
    Query platform, implementation, board and interpreter information.

``EvRepl.getFsStats(path = '/', { signal = null } = {})``
    Query filesystem capacity/free-space information for a path.

``EvRepl.listFs(path, { recursive = false, signal = null } = {})``
    Return flat {name,path,type,size} entries; optionally recurse.

``EvRepl.walkFs({ signal = null } = {})``
    Return the legacy nested filesystem tree.

``EvRepl.readFile(fn, { timeout = null, chunkTimeout = 30000, signal = null, maxBytes = null } = {})``
    Return complete Uint8Array file contents; reject changed, oversized or incomplete reads.

``EvRepl.touchFile(fn)``
    Create/truncate one file; Promise<void>.

``EvRepl.removeFile(path, { signal = null } = {})``
    Remove one file; Promise<void>.

``EvRepl.removeDir(path, { signal = null } = {})``
    Remove an empty directory; Promise<void>.

``EvRepl.makePath(path)``
    Create a path through the legacy REPL helper.

``EvRepl.writeFile(fn, data, chunk_size = EV_REPL_FILE_CHUNK_BYTES, direct = false, { signal = null } = {})``
    Legacy REPL byte/chunk helper; application files.write and script staging use the binary engine, never this helper.

``EvRepl.readSysInfoMD()``
    Return formatted system information as Markdown.

evReplExecInputBytes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``evReplExecInputBytes(source)``

Return the UTF-8 byte count of the escaped REPL exec command, including command overhead.

@esp-vision/device-core/session
--------------------------------------------------------------

DeviceSession
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceSession({ transport, EvMuxParser, makeEvMuxFrame, EvRepl, handlers = {}, sid = 'ide-client', sleep = defaultSleep, })``

Low-level v4 interpreter. Inject transport, EvMuxParser, EvRepl and makeEvMuxFrame; handlers receives text/rawText/log/preview/rpc/diagnostic and session lifecycle callbacks. The caller subscribes transport input and feeds bytes itself; constructing this class does not open a connection. Application createDevice does this lifecycle work for you.

``DeviceSession.feedBytes(bytes)``
    Feed received byte chunks into the owned parser.

``DeviceSession.feedText(text)``
    Feed compatibility text input.

``DeviceSession.hasFeature(feature)``
    Return whether negotiated capabilities advertise a feature.

``DeviceSession.writeFrame(channel, type, method, payload, { authorize = null, vmRequest = null, seq: requestedSeq = null, signal = null, writeTimeout = null, } = {})``
    Encode/send a frame with current session/epoch context; returns the transport Promise.

``DeviceSession.rpc(method, payload = {}, { timeout = 4000, channel = null, signal = null, binaryResponse = false } = {})``
    Send a correlated RPC, awaiting JSON or binaryResponse; reject timeout/cancellation/firmware error.

``DeviceSession.captureImage(method, payload = {}, options = {})``
    Call a capture method and return JPEG {data,mimeType,metadata}.

``DeviceSession.negotiate(options = {})``
    Probe v4, validate identity/limits, acquire ownership and optionally prove REPL readiness.

``DeviceSession.writeFile(path, content, { encoding = 'utf-8', mode = 'overwrite', runAfterWrite = false, signal = null, onProgress = null, } = {})``
    Write UTF-8/binary through the shared reliable file.write binary engine; verify SHA-256 commit before returning. Requires file.write.binary; no upload fallback.

``DeviceSession.writeScript(path, content, { runAfterWrite = false, signal = null } = {})``
    UTF-8 compatibility wrapper over writeFile.

``DeviceSession.createOperationId()``
    Return a new network-mutation operation ID.

``DeviceSession.networkStatus(options = {})``
    Return network.status state/result.

``DeviceSession.configureNetwork({ ssid, password, operationId = undefined }, options = {})``
    Send credentials and operationId; return accepted ACK, not final connection evidence.

``DeviceSession.setNetworkEnabled(enabled, options = {})``
    Send persisted network enable/disable mutation.

``DeviceSession.forgetNetwork(options = {})``
    Request forgetting persisted credentials.

``DeviceSession.release({ timeout = 1200 } = {})``
    Release the EV-MUX session lease; Promise of RPC result.

``DeviceSession.executePython(source, { timeout = null, signal = null } = {})``
    Execute source through framed REPL and return output.

``DeviceSession.interrupt()``
    Send Ctrl-C through the framed signal channel.

``DeviceSession.control(operation, { timeout = 15000, signal = null } = {})``
    Run stop/soft-reset/hard-reset/bootloader control.

``DeviceSession.stopForRepl({ timeout = 5000, signal = null } = {})``
    Interrupt and wait for an executable REPL; returns boolean readiness.

``DeviceSession.runScript(path, content, { timeout = 10000, signal = null, onStarted = null, maxOutputChars = Number.POSITIVE_INFINITY, trial = null, inlineCompletion = false, } = {})``
    Run a device script; optional content uploads first; await bounded completion.

``DeviceSession.startScript(path, content, { onStarted = null, ...options } = {})``
    Start a script and return a handle with done Promise.

``DeviceSession.scriptRunSnapshot()``
    Return the active run snapshot or null.

``DeviceSession.snapshot()``
    Return session state, role, capabilities, parser/transport statistics and pending RPC count.

``DeviceSession.close(error = new Error('connection closed'))``
    Reject pending work and reset session state/timers; physical transport closing is the host responsibility.

Readable negotiated state includes state, role, identity, capabilities, sessionId, vmEpoch, vmReady, limits, parser and repl. Update it through session methods rather than direct assignment. ``negotiate`` options: role="unknown", drainMs=300, acquire=true, probeRepl=true, expectedDevice=null, signal=null. TCP skips startup drain. ``control`` operation strings are stop/soft-reset/hard-reset/bootloader, distinct from the facade request object. Low-level network mutation methods return accepted ACKs; the facade configure waits for matching operation completion. A typed run trial, if used, requires operation_id UUIDv4, source_path (at most 512 UTF-8 bytes), source_hash (64 lowercase SHA-256 hex digits) and the advertised launcher trial/typed-runner features; inlineCompletion cannot be combined with a trial.

EV_MUX_DEFAULT_LIMITS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Fallback protocol limits; actual operations use negotiated firmware limits. txLargePayloadMax=0 is the initial fallback, not a promise that every device supports a zero-byte image only.

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

Device path UTF-8 byte limit including temporary-name headroom.

.. code-block:: json

   123

EV_MUX_PAYLOAD_LIMIT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Compatibility payload-budget constant; it does not replace the negotiated RX limit.

.. code-block:: json

   1800

EV_MUX_REQUIRED_CHANNELS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Channels required by the v4 capability validator.

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

Features required by the v4 capability validator.

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

Legacy script.write reference constant. SDK 0.3.0 uploads do not use it; binary chunk size comes from fileWriteChunkMax.

.. code-block:: json

   900

EV_MUX_VERSION
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Supported session protocol version.

.. code-block:: json

   4

evmuxRpcChannelForMethod
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``evmuxRpcChannelForMethod(method)``

Return debug.rpc for debug.* methods and user.rpc otherwise.

requiresEvmuxFirmwareUpgrade
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``requiresEvmuxFirmwareUpgrade(err)``

Return boolean for firmware-upgrade errors, including version mismatch or the firmware-update marker.

validateEvmuxCapabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``validateEvmuxCapabilities(payload)``

Validate v4 protocol version, required features/channels and the session lease. Returns void; throws on mismatch or missing capabilities.

@esp-vision/device-core/actions
--------------------------------------------------------------

DEVICE_ACTION_DESCRIPTORS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The 17 descriptors below define host action contracts. They do not mean that platform port discovery, connection UI or workspace deployment has been implemented in Core; bind those handlers explicitly. Inputs follow each descriptor inputSchema; outputSchema permits handler-defined extra properties. risk contains readOnly/destructive/openWorld flags.

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

Schema version of host action descriptors.

.. code-block:: json

   1

DEVICE_ACTORS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Allowed actor names for the host action layer; the ordinary device facade does not require an actor.

.. code-block:: json

   [
     "agent"
   ]

DeviceActionError
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionError(code, message, { recovery = null, details = null, cause = null } = {})``

Structured host-action error; code/message plus optional recovery/details/cause. toJSON omits cause.

``DeviceActionError.toJSON()``
    Return {code,message,recovery?,details?}.

DeviceActionGateway
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionGateway({ registry = new DeviceActionRegistry(), coordinator = new OperationCoordinator(), lease = new DeviceLease(), getSession = () => null, authorize = async () => true, } = {})``

Validates action input and context, checks the host lease/ready session, calls authorize(descriptor, input, context), then coordinates the bound handler. context.actor must be supported and context.requestId must be supplied; context.signal supports cancellation. Host code must supply its actual authorization policy; the default authorize returns true.

``DeviceActionGateway.invoke(actionName, input = {}, context = {})``
    Promise: validated/authorized bound-handler result; errors carry action context.

DeviceActionRegistry
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceActionRegistry(descriptors = DEVICE_ACTION_DESCRIPTORS, { schemaVersion = DEVICE_ACTION_SCHEMA_VERSION } = {})``

Registry of action descriptors and explicitly bound handlers. Descriptors require name, inputSchema, outputSchema and risk. Unbound descriptors are metadata; Gateway.invoke rejects them with ACTION_UNAVAILABLE. register/bind return this for chaining.

``DeviceActionRegistry.register(descriptor, handler = null)``
    Register a descriptor and optional handler.

``DeviceActionRegistry.bind(name, handler)``
    Bind a handler to an existing action.

``DeviceActionRegistry.descriptor(name)``
    Return descriptor or null.

``DeviceActionRegistry.handler(name)``
    Return handler or null.

``DeviceActionRegistry.list()``
    Return descriptor array.

``DeviceActionRegistry.validate(name, input)``
    Validate/default input and return the validated copy.

DeviceLease
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceLease()``

Host-side actor lease, distinct from the firmware EV-MUX session lease. actor must be in DEVICE_ACTORS. Multiple handles for the same actor are reference counted; this object neither acquires nor forcibly steals a device session.

``DeviceLease.acquire(actor)``
    Return {id,actor,release}; a conflicting actor throws.

``DeviceLease.release(id)``
    Release one host lease ID.

``DeviceLease.assertAccess(actor)``
    Throw unless actor owns the host lease.

``DeviceLease.snapshot()``
    Return {state,primary,actors,holders}.

DeviceLeaseConflictError
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceLeaseConflictError(owner, requester)``

DeviceActionError for a host actor conflicting with the current lease owner.

OperationCoordinator
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new OperationCoordinator()``

Serial host operation queue with cancellation and close handling. Unlike the device facade, this adapter layer queues operations. operation receives its AbortSignal.

``OperationCoordinator.run({ actor, requestId, actionName, signal }, operation)``
    Enqueue operation and return its result Promise.

``OperationCoordinator.close(reason = actionAbortError('Device session closed'))``
    Reject queued work and abort active work; no new operations accepted.

``OperationCoordinator.snapshot()``
    Return active and queued actor/request/action identities.

@esp-vision/device-core/bindings
----------------------------------------------------------------

DeviceEvidenceBuffer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new DeviceEvidenceBuffer(limit = MAX_EVIDENCE_ENTRIES)``

Bounded host evidence history. Default constructor limit is 500; snapshot defaults to 100. Values are bounded; applications remain responsible for sensitive-data handling.

``DeviceEvidenceBuffer.append(kind, value, details = null)``
    Append a timestamped bounded value and optional details.

``DeviceEvidenceBuffer.snapshot(limit = 100)``
    Return the latest limited entries.

``DeviceEvidenceBuffer.clear()``
    Clear history.

EPHEMERAL_PYTHON_PATH
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reserved path for the advanced temporary-run controller; ordinary source/start uses its own unique path.

.. code-block:: json

   "/.espvision_agent_run.py"

EphemeralPythonController
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new EphemeralPythonController(session, { evidence = null, generation = 'session' } = {})``

One bounded temporary Python run per controller, using EPHEMERAL_PYTHON_PATH. Constructor session is a DeviceSession. Snapshots include runId/path/status/output/nextOffset/finished/cleaned and optional error/outputTruncated/cleanupError/residualPath; a started run is not proof that cleanup succeeded.

``EphemeralPythonController.start(code, { maxDurationMs = 180000, signal = null } = {})``
    Start bounded temporary source; return the run snapshot and clean up after completion.

``EphemeralPythonController.observe(runId, { waitMs = 1000, outputOffset = 0, signal = null } = {})``
    Wait up to waitMs and return incremental output from outputOffset.

``EphemeralPythonController.stop(runId)``
    Stop the identified run and return its cleanup/status snapshot.

``EphemeralPythonController.close()``
    Stop/clean up the active run when disposing the controller.

bindSessionDeviceActions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``bindSessionDeviceActions(registry, { evidence = new DeviceEvidenceBuffer(), lease = null, getOperations = () => null, getGeneration = () => 'session', } = {})``

Bind generic session/file/Python/capture handlers to a registry and return { evidence, controllerFor }. Host-specific listPorts/connect/disconnect/workspace deployment are not provided by this binding and must be implemented by the host.

@esp-vision/device-core/firmware
----------------------------------------------------------------

FirmwareFlashSession
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new FirmwareFlashSession(loader, image, { reset = () => loader.hardReset(), mode = 'update' } = {})``

ROM-loader adapter, separate from a live EV-MUX session. Each instance has one lifetime. state progresses idle → connecting → connected → flashing → verified → closed, or error → closed. flash always disconnects, so closed alone is not evidence that verification succeeded.

``FirmwareFlashSession.connect(bytes)``
    Validate image, chip/revision/security mode and installed partition-table hash before accepting writes.

``FirmwareFlashSession.flash(bytes, { onProgress = () => {}, onPhase = () => {} } = {})``
    Write the application partition, read back every byte, verify SHA-256 and reset; always disconnect. Progress is 0..100 and onPhase receives status strings.

``FirmwareFlashSession.disconnect()``
    Close loader once; subsequent calls are no-ops.

Image descriptor: evMuxVersion=4, bytes, offset, partitionSize, sha256, chipFamily, chipRevision and partitionTable:{offset,bytes,sha256}. Supply trusted build metadata and an exact Uint8Array; default mode never performs whole-chip erase or partition migration. The injected loader provides initialize(), chipFamily/chipRevision, getSecurityInfo(), runStub(true), hardReset(), disconnect(). The returned stub provides flashData(), readFlash(), readRegister() and writeRegister(). P4 normal-boot latch handling is built in. A reset failure after verified readback is reported through onPhase; it does not undo verification or automatically flash again.


``mode: 'install'`` uses a merged image descriptor: application metadata remains under ``application``, while ``bytes/offset=0/sha256/flashSize/bootloader`` describe the complete image. The partitionTable descriptor remains unchanged. Chip revision bounds must cover both bootloader and application. Install skips existing-layout matching and requires the returned stub to implement ``flashId()`` returning the integer JEDEC ID (capacity exponent in bits 16–23). Existing application-update adapters need no changes; the bundled Node and browser ROM adapters provide flashId. Full installation and data replacement semantics are defined in :doc:`api`.

``firmwareMode(mode = 'update')`` validates and returns update/install; other values throw TypeError. ``validateFirmwareImage(image, bytes, { mode = 'update' } = {})`` also validates embedded bootloader/partition/application ranges and hashes for install mode.

firmwareSha256
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``firmwareSha256(bytes)``

Promise<string>: lowercase SHA-256 hexadecimal digest; requires Web Crypto.

validateFirmwareImage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``validateFirmwareImage(image, bytes, { mode = 'update' } = {})``

Promise<Uint8Array>: validate v4, exact byte size, application partition bounds and SHA-256; returns the supplied bytes. It does not infer or download an approved manifest.

@esp-vision/device-core/node
------------------------------------------------------------------

``tcpTransport({ host, port })``
    Return a passive Node TCP byte adapter. Core opens it during ``device.connect({ role: 'tcp' })``.

``serialTransport({ serial, python })``
    Return the shared Node/Python serial byte adapter. Use an explicit serial device name/path and an interpreter with pyserial (for example ``python3``). Core opens/closes it; choose role ``usj``, ``usb`` or ``uart`` for the actual interface. The adapters carry bytes, not another device session. This Node-only export is available starting in 0.1.1.


``openNodeRom({ port, python?, signal? })``
    Return the built-in ROM adapter without opening the port. Use with ``device.firmware.update({ openLoader: ({ signal }) => openNodeRom({ port, signal }), ... })`` for both installation and application updates. The adapter requires separately installed Python esptool>=5.3,<6; python defaults to python3 on Unix and python on Windows. Core manages initialize, validation, write/readback, reset and disconnect. Available from SDK 0.3.0; it is Node-only, not a runtime serialTransport or a TCP OTA adapter.

@esp-vision/device-core/discovery
------------------------------------------------------------------

ESP_VISION_DISCOVERY_SERVICE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

DNS-SD service queried by the Node provider.

.. code-block:: json

   "_esp-vision._tcp.local"

NetworkDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``new NetworkDiscovery(provider)``

Normalizes a provider with list({timeout, signal}), validates results and sorts by hostname. list uses the same limits/result shape as device.discover; construction does not start a scan.

``NetworkDiscovery.list({ timeout = 3000, signal } = {})``
    Collect normalized records within the discovery timeout; supports AbortSignal.

createBridgeDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``createBridgeDiscovery(connectionUrl, { fetch: fetcher = globalThis.fetch } = {})``

Return NetworkDiscovery backed by the bridge GET /devices?timeout=... endpoint. Accept ws/wss connectionUrl and optional fetch injection. Returned connectionUrl must match the bridge origin and a /device/<32-hex> path.

normalizeDiscoveredDevice
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``normalizeDiscoveredDevice(value)``

Return a normalized record or throw: 12 lowercase MAC hex digits, DNS hostname ≤63 characters, board ≤96, firmwareVersion ≤255, integer evMuxVersion, port 1..65535 and at least one valid IPv4. Deduplicate addresses; compatible means v4.

@esp-vision/device-core/discovery/node
----------------------------------------------------------------------------

createNodeDiscovery
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``createNodeDiscovery({ createMdns, interfaceAddress } = {})``

Return NetworkDiscovery using DNS-SD/mDNS PTR/SRV/TXT/A records for _esp-vision._tcp.local. createMdns optionally injects a multicast-dns-compatible factory; interfaceAddress selects an interface IPv4. Otherwise multicast-dns is dynamically imported. Accepts private/link-local IPv4 candidates, honors TTL and destroys sockets after each scan.

Integration boundaries
--------------------------------------------

Subpath exports are executable implementations and host contracts, not aliases of every facade request. Keep one session coordinator per physical device. Device owners, host actor leases, ROM loader lifetimes and discovery scans have different meanings; use the facade for ordinary device work and an explicit subpath only when implementing that host layer.

FirmwareFlashSession.flash result
------------------------------------------

After verified readback, flash returns ``{ verified: true, needsReset: boolean }``. Reset failure sets needsReset; closed state alone is not verification evidence. Application integrations normally use ``device.firmware.update()`` to include release checks and port handoff.


@esp-vision/device-core/browser
------------------------------------------------------------------

``openBrowserRom({ port, logger = {}, signal? })``
    Unreleased API; absent from npm 0.3.0. Return a passive browser ROM adapter for a selected Web Serial ``SerialPort``. Request the port in the user gesture and release any runtime connection first. ``initialize()`` lazily loads pinned tasmota 9.4.2, opens/detects the chip and exposes ``chipName/chipFamily/chipRevision``; it does not start a stub. The adapter implements ``initialize/getSecurityInfo/runStub/hardReset/disconnect`` for ``firmware.update``. Core checks the approved image and ROM security before starting the stub and owns all write/readback/reset semantics. ``logger`` optionally provides ``log/error/debug`` functions. ``signal`` cancels initialization; it never closes a started write/readback. ``disconnect()`` is idempotent and releases the port; initialization failures also close acquired resources. A disconnected adapter cannot be reused. Use a browser bundler; direct upstream native ESM loading is unsupported. See :doc:`integration` for the complete example. This subpath imports no Node modules; the root and Node paths do not load this browser backend.

Release readers in @esp-vision/device-core/firmware (0.4.0)
------------------------------------------------------------------

``loadOfficialFirmware({ board?, version?, fetch = globalThis.fetch, signal? })``
    Load the official current or exact selected version snapshot through the publication reference; return the manifest with publication history. Validate the entire snapshot before optional board filtering. Configs without publication retain legacy metadata compatibility.

``loadReleaseManifest({ url, sha256, version?, sourceCommit?, boards?, fetch = globalThis.fetch, signal? })``
    Load exactly one immutable official manifest, validate raw-byte SHA-256, identities, complete board set, every firmware descriptor and website/model/MCP source boundaries. Return the manifest object without downloading firmware or opening ROM. Limits, fields and single-BIN update behavior are specified in :doc:`api`.

Browser flash startup
---------------------

The browser ROM adapter resets through the active flash stub after verification. A verified write does not prove that firmware has started: hosts must wait for a fresh runtime handshake before enabling configuration. If ``needsReset`` is true, ``resetError`` carries the reset failure code and message; preserve this diagnostic and offer manual reset only as recovery.
