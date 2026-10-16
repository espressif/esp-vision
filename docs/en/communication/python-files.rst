Interactive Python, scripts and file operations
===============================================

:link_to_translation:`zh_CN:[中文]`

One interpreter, multiple host operations
-----------------------------------------

Friendly REPL, uploaded scripts and Device Core file operations ultimately use one MicroPython VM. A long-running program affects REPL and VM RPCs, but does not automatically stop transport keepalive/network controls.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     ui [label = "Host user action\nterminal / run / file"];
     core [label = "Device Core\npython.input() / files.write()\npython.execute() / control()"];
     text [label = "repl.stdin\nPython bytes"];
     rpc [label = "user.rpc\nscript.write / script.run"];
     stop [label = "repl.signal\n0x03"];
     queue [label = "auth + bounded VM/REPL queues"];
     vm [label = "same MicroPython VM\nREPL / VFS / execfile"];
     output [label = "repl.stdout / stderr\nmarkers + application output"];
     ui -> core;
     core -> text; core -> rpc; core -> stop;
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

Public file API and wire implementation
---------------------------------------

.. list-table:: File interface layers
   :header-rows: 1
   :widths: 30 36 34

   * - Host operation
     - Wire implementation
     - Use case
   * - ``device.files.write({ path, data, overwrite, ... })``
     - Core calls ``script.write`` on ``user.rpc`` with base64 chunks sized to the negotiated RX limit; firmware writes VFS through a temporary file.
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

script.write requests and chunks
--------------------------------

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
