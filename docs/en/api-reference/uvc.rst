uvc — USB webcam output
=======================

:link_to_translation:`zh_CN:[中文]`

On boards whose build enables native USB-device UVC, CDC, MSC and UVC are exposed together at startup. Open the ESP-VISION Camera in a host webcam application. If no application image is available, firmware generates a monochrome ESP mark as a JPEG at the selected mode's dimensions. Opening UVC does not initialize or reconfigure ``sensor``. This module is not enabled on every board.

The released fallback profile advertises one MJPEG mode: 320×240 at 10 FPS, using isochronous transport. JPEG frames must fit within 48 KiB. Target mode tables are opt-in and disabled by default. Their first entry preserves 320×240 at 10 FPS. Configured screen candidates include 240×240 on P4X-EYE and S3-EYE and 1024×600 on P4X Function EV Board. Mosaico candidates include a 480×480 screen-sized target and a 640×480 configured raw-input target, each at 10, 20 and 30 FPS. P4 candidates follow their configured 1280×720 raw-input size. P4X-VISION and AtomS3R-M12 board configuration defines no LCD size, so their tables add no screen candidate. The configured sizes describe the UVC modes offered to the host and may differ from the sensor’s physical limits. At runtime, all UVC builds accept input images whose dimensions differ from the advertised mode, but host compatibility for mismatched dimensions is not guaranteed. Actual delivery also depends on host scheduling and concurrent USB traffic. ``get_mode()`` reports the mode selected by the host; ``is_streaming()`` reports whether the host enabled the streaming interface, not whether a particular application displayed a frame.

UVC keeps one lazily allocated PSRAM buffer for application frames, capped at 48 KiB by the current profiles. The source image or encoder buffer can be reused after a successful call because UVC copies the JPEG into its own buffer. While TinyUSB is sending that application frame, another application frame cannot replace it; the frame remains owned until transfer completion or confirmed cancellation. The generated ESP mark is a separate fallback source, not a second application-frame queue slot.

Boards with candidate tables require both ``MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES=1`` and ``MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE=1`` to enable the experimental modes. The P4X-EYE table includes 320×240, 240×240 and 1280×720 at 10, 20 and 30 FPS. Check host compatibility with the specific firmware, camera workload and USB connection before enabling these modes in a release.

.. py:module:: uvc

.. py:function:: write(img)

   Publish a processed ``image.Image``. Runtime accepts input dimensions that differ from the advertised UVC mode; hosts may reject or mishandle mismatched frames. Returns ``False`` if the host stream is inactive or an application frame is still in flight, and ``True`` once the JPEG has been copied into UVC's independent buffer. Acceptance does not acknowledge host receipt. RGB565, grayscale and JPEG inputs use the shared JPEG path. Invalid inputs or oversized encoded frames raise ``ValueError``; encoding/allocation failures raise ``OSError``. Camera buffers may be reused immediately after return. A pending frame may be replaced, but an in-flight frame is never overwritten.

.. py:function:: set_frame(img)

   Copy a fixed image only while the host has an active UVC stream. Runtime accepts dimensions that differ from the advertised mode, without a host compatibility guarantee. On success, detach the preview source and repeat the copied frame until replaced or reset. Return ``False`` if the stream is inactive or the application buffer is still in flight. Disconnect clears the fixed frame and returns output to the generated ESP mark. Failed updates preserve the previous source.

.. py:function:: bind(source)

   Bind ``"preview"`` to processed images published by ``img.flush()``. UVC works without an IDE session. Runtime accepts preview images whose dimensions differ from the advertised UVC mode, but host compatibility is not guaranteed. Oversized frames or UVC allocation failures are dropped without blocking EV-MUX. ``None`` detaches the preview source and returns output to the generated ESP mark until another application frame is supplied.

.. py:function:: reset()

   Detach the source and return output to the generated monochrome ESP mark. This does not reset the USB controller or disconnect CDC/MSC. VM soft reset also clears the source. A stopped or idle live producer otherwise leaves its last frame repeating.

.. py:function:: is_streaming()

   Return whether the host has enabled streaming and the USB bus is not suspended.

.. py:function:: get_mode()

   Return the mode selected by the host. Before negotiation, the released fallback and every opt-in table start at ``{"format": "MJPEG", "width": 320, "height": 240, "interval_us": 100000}``. The selected size may differ after the host commits another advertised mode.

.. py:function:: set_mode_callback(callback)

   Register a callable to receive a dictionary with the same fields as ``get_mode()`` after each successful host Commit. If a Commit already occurred, registration schedules the latest committed mode once. The callback is deferred to the MicroPython VM task; it never runs inside the USB callback. If several Commit events arrive before the VM dispatches them, only the newest mode is delivered. Callback exceptions are reported by MicroPython and do not change the USB Commit result. Pass ``None`` to unregister. Soft reset clears the callback; ``uvc.reset()`` only changes the frame source.

Examples
--------

**IDE and UVC preview.** Draw on each QVGA camera frame and publish it via ``img.flush()``.

.. literalinclude:: ../../../example/17-UVC/01-preview.py
   :language: python
   :lines: 5-

**Direct camera output.** Select 320×240 in the host app. The script opens the camera on demand and retries each encoded JPEG briefly while UVC is busy.

.. literalinclude:: ../../../example/17-UVC/02-camera-write.py
   :language: python
   :lines: 5-

**Watch mode negotiation.** Print the dimensions and frame interval committed by the host.

.. literalinclude:: ../../../example/17-UVC/03-mode-callback.py
   :language: python
   :lines: 5-

**Fixed color chart.** Generate a picture at the negotiated size and publish one fixed image per stream with ``uvc.set_frame()``. A camera image can be used in its place.

.. literalinclude:: ../../../example/17-UVC/04-fixed-image.py
   :language: python
   :lines: 5-

**App-controlled camera size.** The registered callback records the negotiated dimensions and the application loop configures a matching QVGA or VGA camera mode. Select a size supported by your board’s camera and UVC table.

.. literalinclude:: ../../../example/17-UVC/05-mode-aware-camera.py
   :language: python
   :lines: 5-
