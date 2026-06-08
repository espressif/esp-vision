image -- Image Processing
=========================

:link_to_translation:`zh_CN:[中文]`

.. py:module:: image

The ``image`` module provides the :py:class:`Image` object and the vision
algorithms built on OpenMV ``imlib``: drawing, format conversion, filtering,
color/blob analysis, and feature detection (lines, circles, rects, QR codes,
barcodes, AprilTags). The codec stream type :py:class:`ImageIO` is documented in
:doc:`imageio`. The source of truth for this page is ``stubs/image.pyi``.

Example
-------

.. code-block:: python

   import sensor, image

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   img = sensor.snapshot()
   img.draw_rectangle(10, 10, 50, 50, color=(255, 0, 0))
   for blob in img.find_blobs([(30, 100, 15, 127, 15, 127)]):
       img.draw_cross(blob.cx(), blob.cy())

Constants
---------

Pixel formats: :py:data:`BINARY`, :py:data:`GRAYSCALE`, :py:data:`RGB565`,
:py:data:`BAYER`, :py:data:`YUV422`, :py:data:`JPEG`, :py:data:`PNG`.

Color palettes: :py:data:`PALETTE_RAINBOW`, :py:data:`PALETTE_IRONBOW`,
:py:data:`PALETTE_DEPTH`, :py:data:`PALETTE_EVT_DARK`, :py:data:`PALETTE_EVT_LIGHT`.

Geometry and scaling flags such as :py:data:`AREA`, :py:data:`BILINEAR`,
:py:data:`BICUBIC`, :py:data:`HMIRROR`, :py:data:`VFLIP`, :py:data:`TRANSPOSE`,
:py:data:`ROTATE_90`, :py:data:`ROTATE_180`, :py:data:`ROTATE_270`, the
``SCALE_ASPECT_*`` family, plus the barcode and AprilTag family constants
(``EAN13``, ``CODE128``, ``TAG36H11`` ...). See ``stubs/image.pyi`` for the full
list.

.. py:data:: BINARY
.. py:data:: GRAYSCALE
.. py:data:: RGB565
.. py:data:: JPEG

   Common pixel format constants.

The Image Class
---------------

.. py:class:: Image(path, *, copy_to_fb=False)
              Image(array, *, buffer=None, copy_to_fb=False)

   An image backed by a pixel buffer. Created from a file path, from an array,
   or returned by :py:func:`sensor.snapshot`.

   **Basic properties**

   .. py:method:: width()
   .. py:method:: height()
   .. py:method:: format()
   .. py:method:: size()
   .. py:method:: bytearray()

   .. py:method:: get_pixel(x, y, *, rgbtuple=True)
   .. py:method:: set_pixel(x, y, pixel)

   .. py:method:: flush()

      Push the image to the host preview over USB CDC.

   .. py:method:: save(path, *, roi=None, quality=50)

      Save the image to ``path`` (format inferred from the extension).

   **Conversion**

   .. py:method:: copy(**kwargs)
   .. py:method:: crop(**kwargs)
   .. py:method:: scale(**kwargs)
   .. py:method:: compress(**kwargs)
   .. py:method:: to_grayscale(**kwargs)
   .. py:method:: to_rgb565(**kwargs)
   .. py:method:: to_jpeg(**kwargs)
   .. py:method:: to_png(**kwargs)

   **Drawing**

   .. py:method:: draw_line(x0, y0, x1, y1, color=..., thickness=1)
   .. py:method:: draw_rectangle(rect, color=..., thickness=1, fill=False)
   .. py:method:: draw_circle(circle, color=..., thickness=1, fill=False)
   .. py:method:: draw_cross(x, y, color=..., size=5, thickness=1)
   .. py:method:: draw_arrow(line, color=..., size=10, thickness=1)
   .. py:method:: draw_image(image, x=0, y=0, **kwargs)

   **Filtering and morphology**

   .. py:method:: invert()
   .. py:method:: erode(ksize, *, threshold=0, mask=None)
   .. py:method:: dilate(ksize, *, threshold=0, mask=None)
   .. py:method:: blur(ksize, **kwargs)
   .. py:method:: gaussian(ksize, **kwargs)
   .. py:method:: median(ksize, **kwargs)
   .. py:method:: morph(ksize, kernel, **kwargs)
   .. py:method:: histeq(*, adaptive=False, clip_limit=-1, mask=None)

   **Analysis and feature detection**

   .. py:method:: get_histogram(thresholds=None, **kwargs)
   .. py:method:: get_statistics(thresholds=None, **kwargs)
   .. py:method:: find_blobs(thresholds, **kwargs)
   .. py:method:: find_lines(**kwargs)
   .. py:method:: find_circles(**kwargs)
   .. py:method:: find_rects(**kwargs)
   .. py:method:: find_qrcodes(**kwargs)
   .. py:method:: find_barcodes(*, roi=None)
   .. py:method:: find_apriltags(**kwargs)

.. note::

   This page lists the most common members. The full method set, including the
   result tuple types (``blob``, ``line``, ``circle``, ``rect``, ``qrcode``,
   ``barcode``, ``apriltag``, ``statistics``, ``histogram``), is documented in
   ``stubs/image.pyi`` and will be expanded here.
