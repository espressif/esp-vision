Frame format, logical channels and RPC protocol
===============================================

:link_to_translation:`zh_CN:[中文]`

Three version identifiers
-------------------------

``firmware.version`` identifies the Git/release build; ``evMuxVersion`` defines the required session protocol; ``EVMUX/1`` identifies wire framing. Determine compatibility from evMuxVersion/capabilities, then display, record or verify firmware.version. Do not infer protocol from date strings.

Byte layout of a frame
----------------------

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

**Figure 12: H/P define body boundaries; EOF is not a payload delimiter.** JPEG, binary file data and text can contain ``0x1e``, ``0x1f`` and CRLF. Never truncate the body on those bytes.

H/P count UTF-8 or binary **bytes**. A character such as U+4F60 typically occupies three UTF-8 bytes; JavaScript string length is not payload_bytes.

CRC32 covers only payload, not metadata/header. Use standard CRC32, e.g. host Python ``zlib.crc32(payload) & 0xffffffff``, encoded as eight hexadecimal digits. Device frames include payload CRC; incoming ``c=00000000`` skips verification. CRC detects corruption, not identity or final operation completion.

Hosts must handle arbitrary fragmentation and coalescing
--------------------------------------------------------

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

**Figure 13: A streaming parser retains state across reads and delivers only fully validated frames.** Record format/length/CRC/deadline errors and resynchronize in a controlled manner; never pass partial frames into REPL/file writes.

Current device RX limits are 384 metadata bytes and 2048 payload bytes. RX parser deadlines are approximately 3000 ms idle and 10000 ms per complete frame. These limit **host commands received by the device**, not preview image size or host receive timeouts for large images.

Metadata fields
---------------

.. list-table:: Common fields
   :header-rows: 1
   :widths: 23 30 47

   * - Field
     - Type
     - Purpose
   * - ``channel``
     - String
     - Select channel, e.g. ``user.rpc`` or ``repl.stdout``.
   * - ``type``
     - String
     - RPC request ``req``, response ``rsp``, notification ``event`` or data ``data``.
   * - ``seq``
     - uint32
     - Request/response correlation number; identical numbers across channels/types do not complete each other.
   * - ``method``
     - String
     - RPC method or stdio source descriptor; ``c-stdio`` is not an RPC method.
   * - ``encoding``
     - Optional string
     - Semantic hint such as ``json``, ``text`` or ``binary``.
   * - ``sessionId``
     - 32-character lowercase hexadecimal string
     - Current owner authorization/output scope; discovery requests omit tokens.
   * - ``vmEpoch``
     - uint32
     - VM operation/output generation scope.
   * - ``sid``
     - Optional string
     - Logical source/domain such as ``stdio``; not sessionId.
   * - ``ts_ms``
     - Optional integer
     - Device timestamp, not automatically synchronized with host time.
   * - ``contentType``、``width``、``height``
     - Channel-defined
     - Describe image payload; JPEG bytes remain in payload, not JSON.

Metadata and RPC request payload must be complete top-level JSON objects with correctly typed known fields and no duplicates. Unknown extensions may be ignored. Escaped strings require a real JSON parser; trailing garbage is not a valid complete object.

Channels and streams
--------------------

.. list-table:: Wire channels
   :header-rows: 1
   :widths: 22 13 15 50

   * - channel
     - Direction
     - stream
     - Payload / purpose
   * - ``user.rpc``
     - Bidirectional
     - user
     - JSON discovery/session/script/device/network control.
   * - ``debug.rpc``
     - Bidirectional
     - debug
     - Debug RPC; capture success responds directly with binary JPEG.
   * - ``repl.stdin``
     - Host to device
     - user
     - Raw REPL input bytes, authorized by session/epoch with a separate size limit.
   * - ``repl.signal``
     - Host to device
     - user
     - Currently accepts only one byte ``0x03`` for Ctrl-C.
   * - ``repl.stdout``
     - Device to host
     - user
     - Text blocks from Python/REPL/HAL and C stdout.
   * - ``repl.stderr``
     - Device to host
     - user
     - Explicit stderr paths such as C stderr; Python exceptions may use stdout.
   * - ``log.idf``
     - Device to host
     - debug
     - IDF/platform log text.
   * - ``preview.frame``
     - Device to host
     - user
     - JPEG data frames produced by ``img.flush()``.

Channel-to-stream assignment is fixed. Hosts do not bind separate serial ports for preview/stdout/RPC. The owner holds both streams; RPC replies return to the exact request link.

RPC methods and execution layers
--------------------------------

.. list-table:: EV-ATP control plane
   :header-rows: 1
   :widths: 22 30 18 30

   * - channel
     - method
     - Execution layer
     - Description
   * - user.rpc
     - ``hello`` / ``capabilities``
     - transport
     - Discovery; hello can explicitly acquire ownership.
   * - user.rpc
     - ``session.keepalive`` / ``session.release``
     - transport
     - Renew lease, query ready/epoch, release owner.
   * - user.rpc
     - ``file.write``
     - transport / native VFS
     - Verified binary upload; session/VM-scoped without interpreter execution.
   * - user.rpc
     - ``script.write`` / ``script.run``
     - VM
     - Legacy Base64 writer / queue script execution.
   * - user.rpc
     - ``device.control``
     - Primarily transport
     - Dispatch stop/reset/status, network.*, storage.writer.*, launcher status and other actions.
   * - debug.rpc
     - ``transport.state`` / ``route.get``
     - transport
     - Observe state/routes without changing public stream bindings.
   * - debug.rpc
     - ``debug.info``
     - VM, except stats
     - device/memory/sensor/fs.list/fs.read; transport.stats does not require VM.
   * - debug.rpc
     - ``debug.capture_frame``
     - VM
     - Capture one image and return JPEG in this RPC rsp.
   * - debug.rpc
     - ``debug.capture_screen``
     - VM
     - Encode the last LCD RGB565 frame as JPEG in this RPC rsp, without camera calls.
   * - debug.rpc
     - ``route.bind`` / ``route.auto``
     - Rejected
     - v4 returns ``UNSUPPORTED_OPERATION``.

Device Core APIs ``files.read/remove`` and ``python.execute`` do not imply identically named wire RPCs. Their file/Python composition is described in :doc:`python-files`.

``debug.capture_screen`` requests use ``debug.rpc`` with the current session and VM epoch; optional ``quality`` is 1–100. Successful responses retain request seq/method, set ``encoding:binary`` and ``contentType:image/jpeg``, include ``width`` / ``height`` and carry JPEG payload. An absent LCD returns JSON error ``NOT_SUPPORTED``; no valid displayed frame returns ``DISPLAY_NOT_READY``. Never substitute camera output. Core's ``capture({ source: 'screen', format: 'jpeg' })`` uses this RPC; see :doc:`preview-debug`.

Requests, responses and asynchronous events can interleave
----------------------------------------------------------

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

**Figure 14: While waiting for seq=20, the reader must continue handling other complete frames.** Match at least type, seq, channel and method, then session/epoch as appropriate. Even an identically numbered ``event/hello`` cannot serve as ``rsp/hello``.

Acquisition replies may allocate a new session; discovery replies grant no owner authorization. Successful VM replies must match the requested epoch; control replies may report a later current epoch. Some authorization errors omit session fields: handle their codes without accepting returned values as new authorization.

Reject owner ``repl.stdout``, ``repl.stderr`` and ``preview.frame`` output from other sessions or old VMs. Never attribute images/control replies to whichever request was most recent.

A verifiable host encoder
-------------------------

This **host CPython** example explains framing. Use the shared client in :doc:`host-integration` for actual communication.

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

This function only encodes frames. It implements no session, concurrent reader, deadlines, RPC correlation or recovery, and cannot replace a complete client. Do not assume host ``zlib``/thread/socket combinations are available unchanged in device MicroPython.

Authorized file.write requests are the bounded exception: binary payloads can carry 16384 file bytes plus a 20-byte ID/offset prefix, with CRC32. All other control payloads retain the 2048-byte limit. See :doc:`python-files` for begin/data/commit/abort.
