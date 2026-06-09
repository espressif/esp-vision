image -- Image Processing
=========================

:link_to_translation:`zh_CN:[中文]`

The ``image`` module provides the :py:class:`Image` object and the vision
algorithms built on OpenMV ``imlib``: drawing, format conversion, filtering,
color/blob analysis, and feature detection (lines, circles, rectangles, QR
codes, and AprilTags). The codec stream type :py:class:`imageio.ImageIO`
is documented in :doc:`imageio`.

.. only:: esp32p4

   The current ESP32-P4 board profiles also enable the ZXing-C++ barcode
   backend and :py:meth:`image.Image.find_barcodes`.

.. code-block:: python

   import sensor, image

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   img = sensor.snapshot()
   img.draw_rectangle(10, 10, 50, 50, color=(255, 0, 0))
   for blob in img.find_blobs([(30, 100, 15, 127, 15, 127)]):
       img.draw_cross(blob.cx(), blob.cy())

Most in-place methods return the image itself, so operations can be chained:
``img.to_grayscale().gaussian(1).binary([(128, 255)])``.

.. seealso::

   For the theory behind these methods, see :doc:`../concepts/image-model`
   (pixel formats, color spaces, the frame buffer) and
   :doc:`../concepts/image-processing` (filtering, thresholding, feature
   detection).

.. include:: _generated/image.rst
