Interactive Python, scripts and file operations
===============================================

:link_to_translation:`zh_CN:[中文]`

One interpreter, multiple host operations
-----------------------------------------

Friendly REPL, script execution and file queries share one MicroPython VM. Binary upload writes native VFS directly under the same session, VM generation and storage drain gate; it does not execute Python. Core still serializes file and interpreter operations. A long-running program affects REPL and VM RPCs, but does not automatically stop transport keepalive/network controls.

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

**Figure 15: Interactive input, file commands and run commands are separate entry points, without separate interpreter isolation.** The session owner must coordinate them; two callers cannot safely concatenate simultaneous REPL commands.

Interactive Python: framed stdin
--------------------------------

The following metadata/payload form one frame. Token/epoch are placeholders and must come from this acquisition:

.. code-block:: json

   {"seq":10,"channel":"repl.stdin","type":"data","encoding":"text","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

.. code-block:: python

   print("hello from current VM")

Follow Python text with appropriate REPL line endings, e.g. ``\r\n``. ``repl.stdin`` carries Python bytes, not JSON ``{"code":"..."}``. This channel is not RPC and does not return a matching ``rsp`` for every input; hosts correlate execution markers in REPL output.

Capabilities normally report ``replInputMax`` as 511 bytes, the usable capacity of a 512-byte ring. Limit each input frame by actual encoded length: multiline input, Unicode, emoji, escapes and CRLF all consume bytes. The 2048-byte control-payload limit does not apply to stdin.

Device Core constructs Python literals using ASCII escapes and places multiline source in ``exec(...)``, avoiding mismatched UTF-8/source input budgets. **This is client encoding policy, not firmware automatically escaping arbitrary oversized source.**

Ctrl-C, Ctrl-D and stopping programs
------------------------------------

Ctrl-C must use ``repl.signal`` with exactly one payload byte ``0x03``:

.. code-block:: json

   {"seq":11,"channel":"repl.signal","type":"signal","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

.. code-block:: text

   payload hex: 03

With EV-MUX enabled, raw ``0x03`` on physical serial is invalid/incomplete framing, not Ctrl-C. Text "03" and JSON ``3`` are not that byte.

Friendly REPL Ctrl-D (``0x04``) can arrive through stdin to trigger soft reset; Device Core can also request it through framed Python. ``device.control`` action ``stop`` provides a control-plane stop entry, but acceptance does not prove interrupted code has exited.

After stopping, require a new complete marker in the current session/epoch to prove executable REPL. Ctrl-C can be delayed by native calls that do not return promptly. A single RPC ack does not prove camera/file resources were released.

Completion markers and command echo
-----------------------------------

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

**Figure 16: Echo includes the marker string; only the later executed print line proves completion.** Reassemble complete lines across frames. String contains matching alone confuses echo with success.

Use a unique marker per run, current sessionId/vmEpoch, complete-line structure and expected result. Camera tasks also need actual image dimensions or other application evidence. Exceptions can appear on stdout; empty stderr is not proof of success.

Reliable binary file upload
----------------------------------------

SDK 0.2.0 uses one upload engine for Python, model, image and arbitrary binary files. Large Python execution stages its temporary source through this engine before ``script.run``. Public ``files.write`` keeps its signature. Older v4 firmware without ``file.write.binary`` can still be inspected, but upload requires updating firmware; Core never silently switches back to the slow legacy writer. The firmware retains script.write only for previously released clients.

All operations use method ``file.write`` on ``user.rpc``, current sessionId/vmEpoch and request/response seq correlation. Small JSON controls remain capped at 2048 bytes. The host creates a fresh 16-byte random upload ID before begin, so cancellation or a lost begin response can still issue abort for that ID.

.. code-block:: json

   {"action":"begin","path":"/models/example.tflite","mode":"overwrite","totalBytes":2097152,"sha256":"<64 lowercase hex>","uploadId":"<32 lowercase hex>"}
   {"ok":true,"uploadId":"<same ID>","chunkBytes":16384}

Data requests set metadata ``encoding:binary``. Their payload is 16 raw upload-ID bytes, a 4-byte little-endian offset, then 1..16384 raw file bytes. CRC32 covers the complete payload, including the prefix. Non-final blocks are multiples of 4096 bytes. The final block can be shorter. NUL, CRLF, SOF and EOF bytes are ordinary payload; there is no Base64 or Python input interpretation. The response is JSON ``{ok:true,writtenBytes:N}``, checked before sending the next block. Duplicate, skipped or out-of-order offsets reject; this is sequential upload, not random access or automatic replay/resume.

.. code-block:: json

   {"action":"commit","uploadId":"<same ID>"}
   {"ok":true,"bytes":2097152,"complete":true}
   {"action":"abort","uploadId":"<same ID>"}

The device keeps one temporary native file open for the entire transfer. An admitted VFS operation remains held until close, preventing MSC handoff across a live descriptor. It checks length and a streaming SHA-256 against begin metadata, fsyncs, closes and publishes only after verification. Empty files use begin/commit without data frames. The receiver allocates larger frame storage only after verifying a file.write binary request from the active owner; limits for unrelated control RPCs remain unchanged. Raw payload is limited to 16384+20 bytes, independent of the 2048-byte JSON limit.

The upload is tied to the physical link generation, session identity, VM epoch and transition generation. Old owners cannot append to or abort a newer upload. Abort, stale generation, session loss or 30 seconds without file progress close/remove the temporary file; failed close retains the storage drain barrier rather than claiming a clean handoff. Uploads are invalidated on VM reset even though native I/O needs no interpreter execution. No file buffer is permanently reserved at boot.

Existing target data is preserved during transfer and integrity checks. FatFS cannot replace a destination in one rename: replacement retains the old regular file as a backup until the verified temporary file is renamed, and attempts rollback on ordinary publication failure. Power loss between FAT directory updates can leave staging/backup files and is not a journaled atomic replace. A successful commit followed by lost response has unknown completion from the host's perspective; inspect the destination before retrying. SHA-256 detects transport/content mismatch against host input, not malicious-host authenticity.

Upload design and measured performance
---------------------------------------

A file is a byte sequence; its extension does not change transfer semantics. Upload and execution are separate operations, so Python, model and image data share one binary engine. Per-transfer open/hash/close replaces per-chunk open/append/close. Raw 16 KiB blocks reduce framing and round trips, and non-final blocks align to Flash's 4 KiB erase size. This reuses existing EV-MUX framing and ownership without another physical connection, public upload API or transfer window.

On 2026-10-16, P4X-EYE revision 3.2 with ESP-IDF 6.1 firmware ``af41759`` and SDK 0.2.0 development sources produced the following application upload times on the existing Flash filesystem. Times include begin/data/verified commit; separate on-device full-file SHA-256 readback passed after every upload. These are observed samples, not board-independent throughput guarantees.

.. list-table:: P4X-EYE Flash upload samples
   :header-rows: 1
   :widths: 25 25 25 25

   * - Interface
     - 256 KiB legacy writer
     - 256 KiB binary upload
     - 2 MiB binary upload
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

The 256 KiB transfer dropped from 292 data RPCs to 16 data RPCs plus begin/commit. TX+RX application bytes fell from roughly 2.14 to 1.04 times file size. The local Flash/FAT sequential-write baseline was about 55 KiB/s; it is a baseline for the current storage implementation, not a physical USB bandwidth limit. Small files still pay fixed begin/commit and FAT metadata costs. Improving further requires measuring storage and receive scheduling rather than increasing JSON chunk sizes.

Public file API and wire implementation
---------------------------------------

.. list-table:: File interface layers
   :header-rows: 1
   :widths: 30 36 34

   * - Host operation
     - Wire implementation
     - Use case
   * - ``device.files.write({ path, data, overwrite, ... })``
     - Core uses file.write control requests and raw binary blocks on user.rpc, with continuous native VFS writes and verified commit. See the binary contract above.
     - The application upload entry for strings and binary bytes. It does not automatically run Python.
   * - Device Core ``EvRepl.writeFile()``
     - Framed REPL Python ``open`` / ``binascii.unhexlify`` / ``write`` / ``rename``.
     - Advanced low-level helper, not the current application ``files.write()`` path or a firmware RPC named writeFile.
   * - ``device.files.read()`` / ``list()`` / ``remove()`` / ``mkdir()``
     - Framed REPL ``seek/read`` / ``os`` queries/deletion with controlled output parsing.
     - Larger files, result validation and cancellation; still requires executable VM/REPL.
   * - ``debug.info`` scopes ``fs.list`` / ``fs.read``
     - ``debug.rpc`` with bounded JSON/base64 debug replies.
     - Quick diagnostics; current fs.read reads at most the first 384 bytes, not a complete paginated offset download API.

Legacy script.write requests and chunks
---------------------------------------

The following contract is retained for SDK 0.1.x clients only; SDK 0.2.0 never uses it.

.. code-block:: json

   {"seq":20,"channel":"user.rpc","type":"req","method":"script.write","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

   {"path":"/hello.py","encoding":"utf-8","mode":"overwrite","offset":0,"totalBytes":11,"contentBase64":"cHJpbnQoMTIzKQo="}

Example content is 11 bytes: ``print(123)\n``. Generate source bytes first and use ``len(source_bytes)`` for totalBytes; the example below calculates rather than guesses the length.

.. code-block:: python

   import base64
   source = b"print(123)\n"
   payload = {
       "path": "/hello.py", "encoding": "utf-8", "mode": "overwrite",
       "offset": 0, "totalBytes": len(source),
       "contentBase64": base64.b64encode(source).decode("ascii"),
   }

``mode`` accepts ``overwrite`` / ``create`` and ``encoding`` accepts ``utf-8`` / ``binary``. Provide offset/totalBytes together or omit both. Base64 expands data by about 4/3, plus path/JSON overhead: a 2048-byte control payload does not fit 2048 raw file bytes.

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

**Figure 17: script.write uses the target .tmp file before the last chunk.** The first chunk prepares it, later chunks append, and the last chunk renames it. Do not report incomplete uploads as final available files.

The implementation **does not provide general random-offset writes or duplicate-chunk deduplication**. Nonzero offsets append rather than seek. Send sequentially, verify every ack bytes/offset/totalBytes/complete, and do not automatically replay a lost-response append chunk: it may duplicate content.

``complete:true`` confirms the publication path completed, without proving a cryptographic whole-file hash. Verify important files by readback or supported device hashing. ``runAfterWrite`` only attempts to queue execution after completion; it proves neither startup nor successful execution.

Paths are limited by UTF-8 bytes. Current ``MICROPY_ALLOC_PATH_MAX`` plus the ``.tmp`` suffix gives a public script-path budget of 123 bytes, also subject to allowed-character validation. Both host preflight and final firmware checks remain necessary.

Advanced framed REPL helper and public reads
--------------------------------------------

The advanced ``EvRepl.writeFile()`` helper (not ``device.files.write()``) writes 224 raw bytes per chunk, hex-encoding them into Python input that must fit stdin. It writes ``.viper.tmp`` by default, closes it, removes the old target and renames; direct mode writes the destination. This client policy differs from RPC ``<path>.tmp`` and is not a general power-loss-atomic transaction.

Do not run parallel uploads sharing temporary files on one REPL. After cancellation/disconnect, verify file state and clean target handles/staging residues; clearing host promises alone is insufficient.

Reads request roughly 1 KiB raw data per step. The target returns hex text, actual total size and chunk byte count. Validate stable size, matching hex/chunk lengths, premature EOF and total deadline before building the result from actual bytes. A short output burst is not EOF.

The public ``device.files.read()`` total deadline defaults to 30 seconds. IDE may use bounded 60 seconds for verified large-file/TCP scheduling needs. Each subrequest still needs deadlines/cancellation; extending the overall budget does not authorize unbounded waits or replay.

Script execution is separate from upload ACK
--------------------------------------------

.. code-block:: json

   {"seq":30,"channel":"user.rpc","type":"req","method":"script.run","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567","vmEpoch":1}

   {"path":"/hello.py"}

.. code-block:: json

   {"ok":true,"path":"/hello.py","queued":true}

This reply only confirms ``execfile(...)`` was queued in REPL input, not execution completion. Imports, models, cameras and application code can fail or run indefinitely. Continue reading until an application marker, actual preview or explicit shared execution-lifecycle result.

Applications use ``device.python.execute({ path, completion: 'start' })`` for interactive runs and receive a handle with a ``done`` Promise. Core delegates to the session implementation internally; applications do not call ``startScript()`` directly. Ordinary infinite applications should not inherit bounded test-run automatic stopping. Run/Stop UI follows shared execution state rather than replacing internal REPL completion callbacks.

Bounded tests use deadlines, cancellation and confirmed stopping; interactive applications keep running with reader/keepalive active. A Run during upload must be rejected/queued by the coordinator, not interleaved as another script command.

Session lease and storage writer lease differ
---------------------------------------------

USB MSC host writes and MicroPython VFS share one volume. EV-MUX ownership alone does not authorize safe concurrent writes. ``device.control`` actions ``storage.writer.status`` / ``storage.writer.acquire`` / ``storage.writer.release`` provide separate volume write ownership.

They involve volume (flash/sd), operationId, writer leaseId, current session and states including available/held/handoff_ready/quarantined. Handoff closes new I/O admission and drains outstanding operations/handles; failures can quarantine the volume. Query state and perform supported safe handoffs; CDC/MSC coexistence does not provide separate filesystem copies.

Session tokens, writer leaseIds and network operationIds may share formats but cannot substitute for one another. Follow capabilities and actual replies for writer transaction structure; do not assume file RPCs automatically acquire write permission.
