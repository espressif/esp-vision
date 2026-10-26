sensor -- Camera
================

:link_to_translation:`zh_CN:[中文]`

The ``sensor`` module controls the camera and captures frames. It mirrors the OpenMV ``sensor`` API so existing OpenMV scripts port with little change.

Camera Initialization and Continuous Capture
--------------------------------------------

A typical program resets the sensor, selects a pixel format and frame size, lets automatic exposure and white balance settle, and then captures frames continuously:

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=2000)

   while True:
       img = sensor.snapshot()

``sensor.reset()`` applies the board's default camera configuration. ``RGB565`` is suitable for display, drawing, and most AI workflows, while ``GRAYSCALE`` reduces memory and processing cost for many detection algorithms. ``sensor.snapshot()`` returns an :py:class:`image.Image` backed by the reusable frame buffer, so copy the image when it must remain unchanged after the next capture. You can pass a writable buffer large enough for the configured frame as ``sensor.snapshot(buffer)``; the returned image keeps that buffer alive.

Image Orientation and Camera Status
-----------------------------------

Use horizontal mirror and vertical flip to correct the image orientation imposed by the physical sensor installation. ``status()`` provides the active dimensions, pixel format, sensor ID, orientation, and crop information for diagnostics:

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_hmirror(True)
   sensor.set_vflip(False)

   info = sensor.status()
   print("sensor id:", info["id"])
   print("output:", info["width"], "x", info["height"])
   print("ready:", info["ready"])

Mirror and flip settings affect subsequently captured frames. Product code can call ``status()`` after initialization to verify that the camera is ready and that the negotiated output size matches the processing pipeline.

Temporarily Stop Capture
------------------------

The camera stream can be stopped when capture is not required and restarted before the next frame:

.. code-block:: python

   sensor.shutdown(True)
   # Perform work that does not require the camera.
   sensor.shutdown(False)
   sensor.skip_frames(n=3)
   img = sensor.snapshot()

After restarting the stream, discard a few frames if exposure or the incoming frame queue needs to stabilize.

.. note::

   The default ``ESP32_P4X_EYE`` SPI settings support QVGA camera capture and LCD drawing at the same time. SPI color transfers use DMA-capable memory, so raising the camera frame size or the panel transfer size/queue depth can exceed the available contiguous DMA block even when ``gc.mem_free()`` and total free PSRAM remain large. If ``display.Display()`` or ``lcd.write()`` raises ``ESP_ERR_NO_MEM``, drop any ``sensor.snapshot()`` image references, call ``sensor.shutdown()``, initialize and use the LCD, then call ``lcd.deinit()`` followed by ``sensor.reset()`` to resume camera capture.

IPA Runtime Control
-------------------

IPA and ISP control methods are optional firmware features. Check ``hasattr(sensor, "get_ipa")`` or ``hasattr(sensor, "get_isp")`` before using them; these methods are absent when the feature is not built in.

Use ``sensor.set_ipa(False)`` to stop the image processing algorithm (IPA) controller and ``sensor.set_ipa(True)`` to start it again. Initialize the camera before using these methods. ``sensor.get_ipa()`` returns the controller's current state, or ``None`` when runtime control is unsupported. Setting IPA on an unsupported camera raises ``OSError``.

.. code-block:: python

   sensor.reset()
   sensor.skip_frames(time=2000)
   if hasattr(sensor, "get_ipa") and sensor.get_ipa() is not None:
       sensor.set_ipa(False)
       print("IPA:", sensor.get_ipa())
       img = sensor.snapshot()
       img.flush()
       sensor.set_ipa(True)

Changing IPA state briefly stops and restarts capture while preserving the output format, frame size, mirror, and flip settings. Setting the current state again has no effect. ``sensor.reset()`` and restarting after ``sensor.shutdown(True)`` restore the default IPA state.

A failed switch leaves the camera shut down rather than in its previous state: the capture buffers, the scaler, and the camera device itself are all released before ``OSError`` is raised. Call ``sensor.reset()`` to bring the camera back.

Disabling IPA stops its automatic parameter updates; it does not bypass the ISP or clear existing tuning. The image may initially look unchanged. To compare the effect, change the illumination or light source color while previewing, then enable IPA and allow its algorithms to settle again. This switch controls only algorithms declared in the active IPA JSON configuration and supported by the backend. Enabling IPA returns its configured blocks, such as CCM, gamma, and sharpening, to automatic control; manual values are not preserved as overrides. This switch does not disable automatic exposure or white balance implemented inside the image sensor. On ESP32_P4X_EYE, the OV2710 controls exposure and gain automatically inside the sensor, independently of IPA; stopping IPA neither freezes these parameters nor makes manual exposure or gain controls available.

Reading IPA Statistics
----------------------

``sensor.get_ipa_stats(timeout=1000)`` collects one new ISP statistics packet while IPA is running. Check ``hasattr(sensor, "get_ipa_stats")`` before use. Initialize the camera and enable IPA first. ``timeout`` is an integer from 1 through 1000 milliseconds; invalid values raise ``ValueError``. The call returns ``None`` on timeout or when runtime IPA control is unsupported, and raises ``OSError`` for an uninitialized camera, disabled IPA, or a device error.

Each read creates a one-entry statistics queue and releases it before returning, including on timeout. It does not stop capture or alter tuning. Reads briefly add copying work to the controller and may block Python for the timeout duration, so sample periodically rather than on every preview frame. Keep calling ``sensor.snapshot()`` between reads to drain camera capture buffers. The statistics sequence is independent of the Python image returned by ``snapshot()``; the two are not guaranteed to describe the same frame.

The returned dictionary always contains these keys. A statistics block without a valid flag in this packet is ``None``; blocks are not accumulated across packets.

.. list-table:: IPA statistics
   :header-rows: 1
   :widths: 22 78

   * - Key
     - Value
   * - ``seq``
     - 64-bit statistics sequence; may restart after camera/IPA initialization.
   * - ``flags``
     - IPA validity bits: AWB=1, AE=2, HIST=4, SHARPEN=8, AF=16, AWB_SUBWIN=32. These differ from the raw ESP Video AE/AWB bits.
   * - ``ae``
     - 5×5 luminance grid indexed as ``[x][y]``.
   * - ``awb``
     - Dictionary with ``counted`` (white patch count), ``sum_r``, ``sum_g``, and ``sum_b``. A valid result can have zero white patches.
   * - ``awb_subwin``
     - 5×5 grid indexed as ``[x][y]``, each cell with the same fields as ``awb``. Requires backend, chip, and statistics configuration support.
   * - ``hist``
     - 16 pixel counts in hardware histogram segment order; segment boundaries depend on the active ISP configuration.
   * - ``sharpen``
     - Maximum high-frequency pixel value.
   * - ``af``
     - Three window dictionaries, each with ``definition`` and ``luminance``. These are focus statistics, not lens positions.

.. code-block:: python

   sensor.reset()
   sensor.set_ipa(True)
   sensor.skip_frames(time=2000)
   sensor.snapshot()
   stats = sensor.get_ipa_stats(timeout=200)
   if stats is not None:
       print(stats["seq"], stats["ae"], stats["awb"])

For a valid ``awb`` block with ``counted > 0``, dividing each channel sum by ``counted`` yields that statistics source's channel mean. Hardware white-patch statistics are filtered samples, not full-frame RGB means; the OV2710 backend can instead provide the sensor's before-gain channel averages with ``counted=1``. Neither source is a mean of the rendered ``image.Image``.

See ``example/01-Camera/04-IPA/ipa_stats.py`` for periodic statistics reads during preview. This API exposes the ISP inputs consumed by IPA; it does not report inferred color temperature, converged exposure, or other algorithm outputs. Use ``get_isp()`` for supported driver parameter readback.

Manual ISP Control
------------------

After initialization, ``sensor.get_isp(block)`` returns a dictionary of the current driver parameters, or ``None`` when the block is unsupported. A block is unsupported when the camera driver has no getter for it or when the firmware was built for a chip revision that lacks the hardware: ``lsc`` needs ESP32-P4 v1.0 or newer and ``blc`` needs v3.0 or newer, while ``exposure`` is write-only on sensors such as SC2336, so ``sensor.get_isp("exposure")`` normally returns ``None``. The OV2710 driver used by ESP32_P4X_EYE implements neither manual ``exposure`` nor ``pixel_gain``: reads return ``None`` and writes raise ``OSError`` with ``ESP_ERR_NOT_SUPPORTED``. A write-only getter on another sensor does not imply that its setter is unsupported. Call ``sensor.set_ipa(False)`` before writing manual settings. ``sensor.set_isp(block, **params)`` updates one block, keeping unspecified fields from its current configuration. Writes while IPA is running raise ``OSError`` to prevent the algorithms from overwriting manual settings.

.. code-block:: python

   sensor.reset()
   sensor.skip_frames(time=2000)
   sensor.set_ipa(False)

   print(sensor.get_isp("wb"))
   sensor.set_isp("wb", enable=False)
   sensor.set_isp("ccm", enable=False)
   sensor.set_isp("gamma", enable=False)
   sensor.snapshot().flush()

   sensor.set_isp("wb", enable=True, rg=1.8, bg=1.4)
   # On sensors with manual exposure and gain support (not OV2710):
   # sensor.set_isp("exposure", value=10000)  # Microseconds, rounded to 100 us.
   # sensor.set_isp("pixel_gain", value=2.0)  # Gain multiplier.

   sensor.set_ipa(True)

``get_isp()`` reads driver configuration, not a hardware commit acknowledgement or a measurement of the returned image. In particular, ``get_isp("wb")`` reports the ISP white balance stage; it does not report the OV2710 sensor's internal AWB gains. ``enable=False, rg=1.0, bg=1.0`` can therefore coexist with sensor-side AWB. On P4 revisions without a separate white balance stage, the driver combines manual white balance gains with CCM.

Capture buffers may already contain frames produced before a write. Sleeping does not consume those frames. Continue capturing, or call ``sensor.skip_frames(n=3)`` to consume queued frames before evaluating a change. This discards older frames; it is not a hardware commit barrier or a guarantee that automatic sensor controls have settled. A repeated write or a fixed sleep is not a reliable way to establish which configuration produced a frame.

The supported block names and fields follow the tuning parameter vocabulary:

.. list-table:: ISP parameters
   :header-rows: 1
   :widths: 18 82

   * - Block
     - Fields
   * - ``exposure``
     - ``value``: exposure in microseconds, positive. The device control counts 100-microsecond units, so a write is rounded to the nearest 100 microseconds and must then fit the range and step the sensor reports, which is where the usable limits come from. SC2336 at 1280x720 30fps, for example, accepts roughly 200 microseconds through 33.2 milliseconds in 100-microsecond steps.
   * - ``pixel_gain``
     - ``value``: positive gain multiplier, selected from the nearest supported gain menu entry within the sensor's range.
   * - ``demosaic``
     - ``enable``, ``gradient_ratio``: nonnegative gradient ratio within the driver's fixed-point range.
   * - ``wb``
     - ``enable``, ``rg``, ``bg``: red and blue gain multipliers, 0 through 1023/256.
   * - ``lsc``
     - ``enable``, ``lsc_gain_size``, ``gain_r``, ``gain_gr``, ``gain_gb``, ``gain_b``: four gain lists, each with the length returned by ``get_isp("lsc")``; values are 0 through 1023/256. The size is determined by capture dimensions.
   * - ``blc``
     - ``enable``, ``stretch_enable``, ``top_left_offset``, ``top_right_offset``, ``bottom_left_offset``, ``bottom_right_offset``: black level offsets, integers from 0 through 255.
   * - ``bf``
     - ``enable``, ``level`` (2–20), ``matrix``: nine integers from 0 through 15 in row order.
   * - ``sharpen``
     - ``enable``, ``h_thresh``, ``l_thresh`` (0–255, low ≤ high), ``h_coeff``, ``m_coeff`` (0 through 255/32), ``matrix``: nine integers from 0 through 31 in row order.
   * - ``ccm``
     - ``enable``, ``matrix``: nine finite numbers strictly between -4 and 4 in row order.
   * - ``gamma``
     - ``enable``, ``x``, ``y``: two lists of 16 integers from 0 through 255. X must increase from an implicit zero by powers of two; the last point must be 255 and is treated as 256 for the final interval.

Set ``enable=True`` when applying new parameters to a disabled block. Passing only ``enable=False`` disables processing without replacing its stored tables. White balance gains are kept across that disable and come back when the block is enabled again. Gamma readback reports the red channel; setting ``x`` or ``y`` applies the resulting curve to all three channels, while an enable-only update preserves the separate channel curves. LSC tables are copied into camera-owned storage and remain valid across garbage collection and IPA switches; the camera keeps the previously installed table intact until the driver accepts the new one, but the underlying driver can update its cached pointers before reporting a hardware error. A failed device write can therefore leave configuration or hardware state uncertain; reset the camera before continuing after such an error. Fixed-point values and sensor settings may be quantized by the driver.

Unknown blocks, invalid values, and array length mismatches are rejected before applying the update. An unsupported write raises ``OSError``. A device error may leave a partially applied configuration; use ``sensor.reset()`` to reinitialize after such a failure.

The ``example/01-Camera/04-IPA/isp_preview.py`` example configures the output format, disables IPA, then continuously previews while randomizing exposure, gain, white balance gains, and CCM every 2 seconds until Ctrl-C. Each update prints the requested exposure and the driver readback for gain, white balance gains, and CCM. Adjust ``UPDATE_INTERVAL_MS`` and the parameter range constants to change the experiment; a value the active sensor rejects is printed and skipped rather than stopping the preview. Ctrl-C leaves IPA disabled and preserves the last settings.

.. seealso::

   :doc:`../concepts/camera-pipeline` explains how a frame travels from the image sensor to an :py:class:`image.Image`, and :doc:`../concepts/image-model` covers pixel formats and color spaces.

   Runnable examples: ``example/01-Camera/00-Snapshot`` (capture and save) and ``example/01-Camera/03-MJPEG`` (Wi-Fi MJPEG stream).

.. include:: _generated/sensor.rst
