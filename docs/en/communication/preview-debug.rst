Preview, debug capture, backpressure and statistics
===================================================

:link_to_translation:`zh_CN:[中文]`

Preview is a data stream, not a subscription RPC
------------------------------------------------

After application ``img.flush()``, the preview producer encodes JPEG when delivery is needed and sends ``preview.frame`` data to the current user-stream owner. There is no extra ``preview.subscribe`` method or Base64 image via ``print``.

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

**Figure 18: Check delivery eligibility and queue conditions before encoding/submitting images.** Without a valid owner, preview is not encoded for the host; continued camera acquisition remains application behavior.

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
     - Camera snapshot.
     - Last RGB565 frame submitted to display.
   * - Wire type
     - Independent ``type:data`` JPEG.
     - Original RPC ``type:rsp`` carries binary JPEG.
     - Original RPC ``type:rsp`` carries binary JPEG.
   * - Execution
     - Application owns an image and submits preview.
     - VM queue initializes/uses the camera.
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

LCD storage is allocated when the display is initialized and released at deinitialization. SPI color transfers finish before a draw returns, so a later draw cannot overwrite a buffer still borrowed by DMA. Camera snapshot storage is allocated on the first snapshot and shared by Python and Core camera capture; requests of the same size reuse the same pointer. An actual format/size change, camera shutdown/reset or VM soft reset releases it. Neither store reserves a full image at boot. Allocation prefers PSRAM, with the existing general-heap fallback if unavailable; configuration alone does not prove PSRAM initialized successfully.

These addresses are stable only during the current allocation lifetime, not across reset or reconfiguration. Python snapshot images borrow this storage; copy an image before retaining it across another capture. Core returns an independently received JPEG on the host. Camera-driver DMA buffers and JPEG encoder working buffers have separate lifetimes; these two stores are not the entire image-memory budget.
