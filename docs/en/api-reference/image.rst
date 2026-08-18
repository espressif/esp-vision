image -- Image Processing
=========================

:link_to_translation:`zh_CN:[中文]`

The ``image`` module provides the :py:class:`Image` object and the vision algorithms built on OpenMV ``imlib``: drawing, format conversion, filtering, color/blob analysis, and feature detection (lines, line segments, circles, rectangles, QR codes, and AprilTags). The codec stream type :py:class:`imageio.ImageIO` is documented in :doc:`imageio`.

.. only:: esp32p4

   The current ESP32-P4 board profiles also enable the ZXing-C++ barcode backend and :py:meth:`image.Image.find_barcodes`.

Drawing and Color-Blob Tracking
-------------------------------

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=1000)

   img = sensor.snapshot()
   img.draw_rectangle(10, 10, 50, 50, color=(255, 0, 0))
   thresholds = [(30, 100, 15, 127, 15, 127)]
   for blob in img.find_blobs(thresholds, pixels_threshold=80, area_threshold=80):
       img.draw_rectangle(blob.rect(), color=(255, 0, 0), thickness=2)
       img.draw_cross(blob.cx(), blob.cy(), color=(0, 255, 0))

An RGB565 threshold contains six LAB limits: ``(L_min, L_max, A_min, A_max, B_min, B_max)``. ``pixels_threshold`` rejects regions containing too few matching pixels, while ``area_threshold`` rejects regions whose bounding box is too small. Blob coordinates are returned in source-image pixels and can be passed directly to drawing methods.

Filtering and Binary Segmentation
---------------------------------

.. code-block:: python

   sensor.set_pixformat(sensor.GRAYSCALE)
   img = sensor.snapshot()
   img.gaussian(1)
   img.binary([(80, 255)])
   img.erode(1)
   img.dilate(1)
   img.flush()

``gaussian()`` suppresses high-frequency noise before thresholding. ``binary()`` converts matching grayscale values to the active binary value, and the erosion/dilation pair removes isolated pixels and restores the main regions. These methods modify the image in place.

QR Code Recognition
-------------------

.. code-block:: python

   sensor.set_pixformat(sensor.GRAYSCALE)
   img = sensor.snapshot()
   for code in img.find_qrcodes():
       img.draw_rectangle(code.rect(), color=255, thickness=2)
       print(code.payload())

``find_qrcodes()`` returns geometry and decoded payload data. Grayscale input generally reduces processing cost, and a smaller ROI can be supplied when the code is expected in a known part of the frame.

Line, Segment, and Circle Detection
-----------------------------------

.. code-block:: python

   img = sensor.snapshot()
   for line in img.find_lines(threshold=1400, theta_margin=25, rho_margin=25):
       img.draw_line(line.line(), color=255, thickness=2)

   for segment in img.find_line_segments(threshold=50, merge_distance=10):
       img.draw_line(segment.line(), color=255, thickness=2)

   for circle in img.find_circles(threshold=2500, r_min=8, r_max=80, r_step=4):
       img.draw_circle(circle.x(), circle.y(), circle.r(), color=255, thickness=2)

``find_lines()`` and ``find_circles()`` use Hough accumulator thresholds. ``find_line_segments()`` uses the Edge Drawing Lines detector, where ``threshold`` is the minimum Sobel gradient magnitude and ``merge_distance`` joins nearby segments. Increasing thresholds reduces weak detections; constraining the radius range, ROI, or image resolution lowers computation and usually improves stability.

Encode and Save an Image
------------------------

.. code-block:: python

   import image

   img = sensor.snapshot()
   jpg = img.to_jpeg(quality=85, subsampling=image.JPEG_SUBSAMPLING_420)
   jpg.save("/sdcard/snapshot.jpg")
   print("encoded bytes:", jpg.size())

JPEG quality trades encoded size for detail, and 4:2:0 chroma subsampling usually produces the smallest color image. Saving requires the destination filesystem to be mounted and writable.

Most in-place methods return the image itself, so operations can be chained: ``img.to_grayscale().gaussian(1).binary([(128, 255)])``.

.. seealso::

   For the theory behind these methods, see :doc:`../concepts/image-model` (pixel formats, color spaces, the frame buffer) and :doc:`../concepts/image-processing` (filtering, thresholding, feature detection).

   Runnable examples: ``example/02-Image-Processing`` (drawing, filters, color tracking, frame differencing), ``example/04-Barcodes`` (QR codes), and ``example/05-Feature-Detection`` (AprilTags, lines, line segments, circles).

.. include:: _generated/image.rst
