uvc — USB webcam output
=======================

:link_to_translation:`zh_CN:[中文]`

Boards with USB-OTG device support expose CDC, MSC and UVC together at startup. Open the ESP-VISION Camera in a host webcam application. The default image is the built-in ESP-VISION logo; opening UVC does not initialize or reconfigure ``sensor``. Boards that disable USB device support do not provide this module.

The initial mode is MJPEG, 320×240, 10 FPS with isochronous transport. JPEG frames must fit within 48 KiB. Actual delivery depends on host scheduling and concurrent USB traffic. ``get_mode()`` reports the supported mode; ``is_streaming()`` reports whether the host enabled the streaming interface, not whether a particular application displayed a frame.

.. py:module:: uvc

.. py:function:: write(img)

   Publish a processed QVGA ``image.Image``. Returns ``False`` if the host is not streaming and ``True`` once the frame has been copied into firmware-owned storage. Acceptance does not acknowledge host receipt. RGB565, grayscale and JPEG inputs use the shared JPEG encoder. Invalid dimensions or oversized encoded frames raise ``ValueError``; encoding/allocation failures raise ``OSError``. Camera buffers may be reused immediately after return. New pending frames replace older pending frames; an in-flight frame is never overwritten.

.. py:function:: set_frame(img)

   Copy a fixed QVGA image even without an open host. Detach the preview source and repeat the copied frame until replaced or reset. Returns ``True`` on success. Failed updates preserve the previous source.

.. py:function:: bind(source)

   Bind ``"preview"`` to the processed images published by ``img.flush()``. UVC and EV-MUX share one JPEG encode and have independent transport readiness. UVC works without an IDE session. Images of other dimensions are ignored by UVC while remaining eligible for EV-MUX. Oversized frames or UVC allocation failures are dropped without blocking EV-MUX. ``None`` detaches and restores the logo. Binding also starts from the logo until a matching frame is published.

.. py:function:: reset()

   Detach the source and restore the logo. This does not reset the USB controller or disconnect CDC/MSC. VM soft reset also restores the logo. A stopped or idle live producer otherwise leaves its last frame repeating.

.. py:function:: is_streaming()

   Return whether the host has enabled streaming and the USB bus is not suspended.

.. py:function:: get_mode()

   Return ``{"format": "MJPEG", "width": 320, "height": 240, "interval_us": 100000}``. Only this mode is advertised and accepted.

A processed camera application can publish to both outputs:

.. code-block:: python

   import sensor
   import uvc

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   uvc.bind("preview")
   try:
       while True:
           img = sensor.snapshot()
           img.draw_rectangle((20, 20, 100, 80), color=(255, 0, 0))
           img.flush()
   finally:
       uvc.reset()

Direct ``uvc.write(img)`` is also available when EV-MUX preview is unnecessary. Do not call both publishing paths for the same frame. No Python callback or camera capture executes inside USB callbacks.
