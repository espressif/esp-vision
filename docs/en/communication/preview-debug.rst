Preview, debug capture, backpressure and statistics
===================================================

:link_to_translation:`zh_CN:[中文]`

Applications explicitly publish preview frames
-----------------------------------------------

After application ``img.flush()``, the preview producer encodes JPEG when delivery is needed and sends ``preview.frame`` data to the current user-stream owner. Read-only TCP observers may subscribe independently as described below. Bound USB webcam output is another independent consumer; see :doc:`../api-reference/uvc`. It does not acquire an EV-MUX control session or initialize the camera.

UVC uses isochronous USB transfers. A failed or short payload aborts that frame: firmware does not append later JPEG payloads to its incomplete prefix. It attempts one header-only UVC ``ERR|EOF`` terminator, reports failure separately from complete-frame transmission, and releases the source slot. The next frame starts with a new FID and clear error flags. Bus reset or alternate-setting cancellation also releases the old source. This cannot guarantee lossless USB delivery; acceptance must decode every retained UVC frame, including concurrent CDC/TCP operation, rather than check only that the output file is nonempty.

.. code-block:: python

   import sensor
   import time

   sensor.reset()
   try:
       sensor.set_pixformat(sensor.RGB565)
       sensor.set_framesize(sensor.QQVGA)
       while True:
           img = sensor.snapshot()
           img.flush()
           time.sleep_ms(200)
   finally:
       sensor.shutdown()

The host explicitly interrupts the infinite loop; ``finally`` supplies resource cleanup. This device example guarantees neither delivery nor frame rate. Hosts must verify complete JPEG and session/epoch.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     image [label = "sensor.snapshot()\nimage.Image"];
     flush [label = "img.flush()"];
     gate [label = "owner / handshake / vmReady\npreview queue availability"];
     jpg [label = "JPEG encode"];
     newest [label = "latest pending preview\nreplace stale pending frame"];
     mux [label = "preview.frame\nbinary payload + CRC"];
     host [label = "verify scope + decode JPEG\nrender latest frame"];
     image -> flush -> gate -> jpg -> newest -> mux -> host;
   }

**Figure 18: Check delivery eligibility and queue conditions before encoding/submitting images.** Without an owner, observer or bound UVC consumer, preview is not encoded; continued camera acquisition remains application behavior.

An image frame
--------------

Conceptual metadata follows; actual seq, route, timestamp, sessionId and vmEpoch come from the current TX snapshot:

.. code-block:: json

   {
     "sid": "preview", "seq": 40, "channel": "preview.frame",
     "type": "data", "encoding": "binary", "route": "usb",
     "contentType": "image/jpeg", "width": 160, "height": 120,
     "ts_ms": 123456,
     "sessionId": "89abcdef0123456789abcdef01234567", "vmEpoch": 1
   }

Payload begins directly with JPEG binary bytes. p is encoded JPEG size, not width times height or source RGB565 buffer size. Do not send JPEG payload to JSON/UTF-8 parsers.

``seq`` helps observe frame order, not request retransmission of missing preview frames. Render the latest complete image; discard other-session/old-epoch images to prevent showing the previous program after soft reset.

Preview, camera capture and LCD screenshots
-------------------------------------------

.. list-table:: Three explicit image sources
   :header-rows: 1
   :widths: 25 25 25 25

   * - Property
     - ``preview.frame``
     - ``debug.capture_frame``
     - ``debug.capture_screen``
   * - Trigger
     - Application ``img.flush()``.
     - Host camera-capture request.
     - Host LCD-screenshot request.
   * - Source
     - Application-submitted image.
     - Last camera acquisition published by the application.
     - Last RGB565 frame submitted to display.
   * - Wire type
     - Independent ``type:data`` JPEG.
     - Original RPC ``type:rsp`` carries binary JPEG.
     - Original RPC ``type:rsp`` carries binary JPEG.
   * - Execution
     - Application owns an image and submits preview.
     - VM queue reads an independent immutable acquisition copy.
     - VM queue reads the display buffer and encodes without camera calls.
   * - Delivery
     - Lossy, newest pending frame preferred.
     - Correlated by request seq/channel/method, session and epoch.
     - Same RPC correlation, or an error if no image is available.

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Device [label = "debug.rpc req: capture_screen, seq=50"];
     Device -> VM [label = "last display RGB565 frame / JPEG encode"];
     Host <-- Device [label = "debug.rpc rsp, seq=50\nencoding=binary, image/jpeg"];
     Application -> Device [label = "img.flush()"];
     Host <-- Device [label = "preview.frame data\nnot capture RPC response"];
   }

**Figure 19: One-shot capture is delivered by its original RPC response, without waiting for another image channel.** Core exposes ``capture({ source: 'camera' | 'screen', format: 'jpeg' })`` and never substitutes one source for the other. Successful results contain ``{ data, mimeType, metadata }`` with dimensions in metadata. Failures may be JSON errors; check encoding/contentType before decoding.

Before transmission, the shared JPEG encoder checks that its reported output length is positive and fits the allocated buffer, and validates the complete image structure against the source dimensions. This applies to camera capture, LCD capture and preview. Invalid encoder output is never queued as an image; capture returns ``JPEG_FAILED``. Core validates JPEG structure and metadata dimensions after EV-MUX length, EOF and CRC checks and session/epoch filtering. An invalid response rejects only its matching capture with ``INVALID_IMAGE``; an invalid preview is discarded with a diagnostic and never reaches image callbacks.

The image checks cover self-contained 8-bit Huffman JPEG headers, bounded marker segments and table references, scan boundaries and final EOI without trailing bytes. They do not decode entropy-coded pixels. Full decoder verification remains part of hardware acceptance. A disconnected link can still leave partial bytes on the wire; the receiver must wait for a complete validated EV-MUX frame and discard incomplete input, rather than display it.

Camera capture is passive and requires the advertised ``debug.capture_frame.observed.v1`` feature. It encodes the independent copy published after the application's last successful camera acquisition, including ``sensor.snapshot(buffer)`` with caller-provided storage and frames acquired by ``sensor.skip_frames()``. It never initializes the sensor, takes another physical frame, writes the application's ``Image`` or follows a caller buffer after that acquisition returns. Later application drawing is visible through ``img.flush()`` preview or the LCD buffer, not through this raw-acquisition source. Device Core 0.6 refuses the unsafe camera RPC on firmware missing this feature; upgrading Core alone does not add safe firmware capture.

No valid publication returns ``CAMERA_FRAME_NOT_READY``: before the first application acquisition, after an acquisition failure, or after camera reconfiguration/shutdown/reset or VM soft reset. An idle camera is not started to satisfy the request. A real change to frame dimensions, pixel format, mirror or flip invalidates the prior frame before changing hardware; a failed change may have altered the device and also leaves that frame invalid. Requests that leave these settings unchanged retain the current frame. An actual IPA mode change also invalidates before the controller stops/restarts capture, including a failed restart; an identical mode retains the frame. Manual ISP writes conservatively invalidate before accessing hardware, including partial failures, even if the requested values are unchanged. These changes also release any observation allocation-failure latch. The next successful application acquisition publishes the new dimensions/format. Camera observation and LCD capture never substitute for each other.

Camera publication, camera state mutations and JPEG encoding run on the main VM task. Python worker-task mutations are rejected with ``OSError`` instead of racing acquisition, encoding or cleanup. The transport task only queues requests; it never holds camera pixels. A queued request may expire with ``VM_BUSY`` / ``VM_TIMEOUT``; cancellation, owner loss or an epoch change can discard its response without taking a frame or changing application storage. Soft reset stops request admission before it releases both camera stores. A hardware reset recreates them empty.

LCD capture does not initialize the screen, redraw it or start the camera. It reads only the last valid RGB565 display frame. An absent LCD returns ``NOT_SUPPORTED``; an uninitialized display or absent valid frame returns ``DISPLAY_NOT_READY``. Reading and encoding run in the VM task, serialized with display operations and soft-reset cleanup. Busy VM work may return ``VM_BUSY`` / ``VM_TIMEOUT``. Optional JPEG ``quality`` is 1–100. The result represents software display state rather than physical panel readback; it cannot prove backlight or panel hardware operation.

Use ``--screen-output`` on the unified Core hardware CLI to display a test pattern and save a JPEG. See :doc:`validation` for commands and acceptance criteria.

Queue priorities and dropped frames
-----------------------------------

Control, session, REPL, log and preview use distinct TX classes. Ordinary small-frame budgets are bounded and control has reserved capacity. A non-reading host cannot establish that all output is lossless.

Preview retains only the newest pending frame, replacing older unsent images. Already-transmitted bytes cannot be withdrawn or combine two JPEG tails. Preview and large debug responses share at most three demand-allocated 256 KiB buffers; this neither holds three arbitrary images nor permits 256 KiB host commands.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     f1 [label = "pending F1"];
     f2 [label = "new F2 replaces F1"];
     f3 [label = "new F3 replaces F2"];
     send [label = "TX sends latest F3"];
     read [label = "host continuously reads"];
     f1 -> f2 -> f3 -> send -> read;
   }

**Figure 20: Limiting stale-preview latency takes priority over delivering every image.** Saving all images requires device storage or a dedicated recording interface with separately designed capacity, playback and acceptance. Preview supplies no such guarantee.

UART deadlines depend on baud rate and total frame bytes; USB/TCP also use bounded waits. Writes may accept partial bytes or return WOULD_BLOCK; TX must retain the unsent suffix and continue within deadline. TX/socket failures can close TCP links; an RX partial-frame timeout only resets the parser of an otherwise live connection and does not imply device reset.

Log channels and system-log UI
------------------------------

IDE can combine ``log.idf``, RPC diagnostics and connection events into a system-log panel, but that panel is not a ninth firmware channel. ``repl.stdout`` primarily displays application/REPL text; source mappings are in :doc:`architecture`.

Hosts may choose formatting, colors and filters without changing firmware ownership/routing. Identify unowned default/fallback logs, raw startup console and current-session application output separately in diagnostics.

Interpreting counters
---------------------

``debug.info`` scope ``transport.stats`` and ``transport.state`` remain queryable while Python is busy, exposing queues, RX rejections, TX timeouts, preview drops and backend state. Distinguish endpoint/link counters, global counters and application completion:

* Unread default USJ output can raise global drops without proving a particular active TCP RPC failed.
* A complete correlated RPC reply does not prove whole-application completion.
* Preview drops are permitted realtime behavior and do not automatically invalidate file transfer; accepting wrong-owner/epoch images is a protocol error.
* Reset/generation changes can alter counter scope; record sampling time, endpoint and session identity.

Keep diagnostics bounded to necessary counters/state. Do not flood the same TX queues with printing from failure paths.

Frame storage lifetime
----------------------

LCD storage is allocated when the display is initialized and released at deinitialization. SPI color transfers finish before a draw returns, so later drawing cannot overwrite a buffer still borrowed by DMA. The application's default camera snapshot buffer is allocated on its first use; successive application captures may reuse that buffer, while an explicit caller buffer belongs to the application. Python must still copy an image it wants to retain across its own next capture or camera reconfiguration.

Safe camera observation owns a separate raw-frame copy, allocated after the first successful application acquisition and reused for stable geometry. For example, QVGA RGB565 adds 153,600 bytes to the application snapshot storage; it does not reserve memory at boot and does not allocate a new buffer per host request. The copy prefers PSRAM and falls back to an 8-bit-capable internal heap; configuration alone does not prove PSRAM initialized. Only CPU-owned output is copied: board backends finish their DMA/PPA operations and cache synchronization before publishing. No observation buffer is submitted directly to camera DMA.

A failure to allocate the observation copy preserves application capture success and pixels. Camera observation then returns ``NO_MEM`` until this allocation-failure state is released, even if a later individual acquisition fails; allocation is not retried on each subsequent frame. A camera reconfiguration/shutdown/reset or VM soft reset releases this state so a later application acquisition can retry. Actual reconfiguration releases the observed copy before touching hardware, and VM cleanup also releases the default application buffer. A borrowed firmware observation view remains valid only during that VM call; Core returns independently received JPEG bytes on the host. Driver DMA, PPA, JPEG and transport buffers have separate lifetimes and must be included in the total image-memory budget.

Read-only TCP Preview
---------------------

A preview client may connect to the same TCP port (3232 by default) and send ``user.rpc/preview.subscribe`` with payload ``{}`` within the 3000 ms handshake deadline. It must not acquire a control session first. The response is ``{"ok":true,"subscribed":true,"channel":"preview.frame","maxObservers":2}``. ``preview.unsubscribe`` with ``{}`` stops publication on that connection. Both operations are idempotent and do not require ``sessionId`` or ``vmEpoch``. Capabilities advertise ``preview.observers.v1``, ``preview.subscribe`` and ``preview.unsubscribe``.

Up to two observers can watch alongside the exclusive USB, USJ, UART or TCP controller. A third observer receives ``OBSERVER_LIMIT``; one of the three TCP slots remains available for control. An observer's ``hello`` returns ``READ_ONLY``; Python, files, REPL and device control remain subject to owner authorization. To become a controller, use a separate connection. Observers receive only the explicitly published image channel and replies to their own requests; preview metadata does not expose the controller's session ID or execution identity. Its ``route`` field retains the current user-stream route; the observer socket is independent. This role separation does not add network authentication.

Applications publish processed images with ``img.flush()``. Connecting does not start sensor capture. The existing JPEG preparation is shared with the control preview and bound UVC output; no HTTP server or additional JPEG encoding is introduced. Observers share one immutable JPEG copy and one CRC calculation per publication, with independent TX cursors. TCP coalesces the frame header, metadata, JPEG and trailer with scatter/gather ``sendmsg`` rather than issuing a separate send for every segment. It still sends a separate byte stream to each observer, so bandwidth grows with the number of viewers. The existing owner preview pool remains separate.

RX parsers are allocated statically for real endpoint slots rather than a rectangular transport/slot table, avoiding unused serial parsers as TCP capacity grows. Temporary TCP send memory exhaustion is treated as backpressure within the same bounded deadline. Socket close runs outside the shared I/O lock, with a short send timeout bounding lwIP FIN-allocation retries. ``debug.info`` with ``scope:transport.stats`` includes ``lastTxTimeout`` (transport, slot, byte offset, total bytes, elapsed milliseconds and idle milliseconds) for diagnosis.

At most three lazily allocated, reusable 256 KiB PSRAM slots serve observers (768 KiB maximum), with two small frame descriptors per observer. Frames that have not started may be replaced by the latest publication. Read-only frames have an 8000 ms absolute deadline and a 4000 ms no-progress deadline, advertised as ``tcp.observerFrameTimeoutMs`` and ``tcp.observerIdleTimeoutMs``. Progress refreshes the idle timer; replacing a frame before its first byte does not refresh either timer. Started frames finish within those budgets or their socket is closed; a slow observer does not hold the controller or another observer's queue. Generation checks prevent old frames or subscriptions from reaching a reconnected socket. Subscribed sockets need no control lease or keepalive and remain subscribed while idle; ``preview.unsubscribe`` returns the socket to normal idle reclamation.

The host helper uses Python's standard library and its bounded EV-MUX parser. ``--view`` additionally requires ``ffplay`` on PATH; its bounded display queue keeps only the latest pending image. The wire parser retains frame boundaries and validates JPEG CRCs.

.. code-block:: console

   python tools/preview_tcp.py 192.168.1.100 --view
   python tools/preview_tcp.py 192.168.1.100 --duration 10 --save-last preview.jpg

Read-only acceptance can run explicitly with ``tools/test_preview_tcp.py --host <device-ip> --port <serial-port> --device-cli <installed-plugin>/cli/device-cli.mjs --output-dir <project-local-directory>``. Add ``--video <V4L2-node>`` to check concurrent UVC. This test replaces the running Python application with bounded test programs, uses Pillow to decode received JPEGs, and restores the UVC logo afterward.
