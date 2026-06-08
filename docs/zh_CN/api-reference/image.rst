image -- 图像处理
=================

:link_to_translation:`en:[English]`

.. py:module:: image

``image`` 模块提供 :py:class:`Image` 对象以及基于 OpenMV ``imlib`` 的视觉算法：绘图、
格式转换、滤波、颜色/色块分析，以及特征检测（直线、圆、矩形、二维码、条形码、AprilTag）。
编解码流类型 :py:class:`ImageIO` 见 :doc:`imageio`。本页内容以 ``stubs/image.pyi`` 为准。

示例
----

.. code-block:: python

   import sensor, image

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   img = sensor.snapshot()
   img.draw_rectangle(10, 10, 50, 50, color=(255, 0, 0))
   for blob in img.find_blobs([(30, 100, 15, 127, 15, 127)]):
       img.draw_cross(blob.cx(), blob.cy())

常量
----

像素格式：:py:data:`BINARY`、:py:data:`GRAYSCALE`、:py:data:`RGB565`、
:py:data:`BAYER`、:py:data:`YUV422`、:py:data:`JPEG`、:py:data:`PNG`。

调色板：:py:data:`PALETTE_RAINBOW`、:py:data:`PALETTE_IRONBOW`、
:py:data:`PALETTE_DEPTH`、:py:data:`PALETTE_EVT_DARK`、:py:data:`PALETTE_EVT_LIGHT`。

几何与缩放标志，如 :py:data:`AREA`、:py:data:`BILINEAR`、:py:data:`BICUBIC`、
:py:data:`HMIRROR`、:py:data:`VFLIP`、:py:data:`TRANSPOSE`、:py:data:`ROTATE_90`、
:py:data:`ROTATE_180`、:py:data:`ROTATE_270`、``SCALE_ASPECT_*`` 系列，以及条形码与
AprilTag 系列常量（``EAN13``、``CODE128``、``TAG36H11`` 等）。完整列表见
``stubs/image.pyi``。

.. py:data:: BINARY
.. py:data:: GRAYSCALE
.. py:data:: RGB565
.. py:data:: JPEG

   常用像素格式常量。

Image 类
--------

.. py:class:: Image(path, *, copy_to_fb=False)
              Image(array, *, buffer=None, copy_to_fb=False)

   以像素缓冲为底层的图像。可由文件路径、数组创建，或由 :py:func:`sensor.snapshot`
   返回。

   **基本属性**

   .. py:method:: width()
   .. py:method:: height()
   .. py:method:: format()
   .. py:method:: size()
   .. py:method:: bytearray()

   .. py:method:: get_pixel(x, y, *, rgbtuple=True)
   .. py:method:: set_pixel(x, y, pixel)

   .. py:method:: flush()

      通过 USB CDC 将图像推送到主机预览。

   .. py:method:: save(path, *, roi=None, quality=50)

      将图像保存到 ``path``\ （格式由扩展名推断）。

   **格式转换**

   .. py:method:: copy(**kwargs)
   .. py:method:: crop(**kwargs)
   .. py:method:: scale(**kwargs)
   .. py:method:: compress(**kwargs)
   .. py:method:: to_grayscale(**kwargs)
   .. py:method:: to_rgb565(**kwargs)
   .. py:method:: to_jpeg(**kwargs)
   .. py:method:: to_png(**kwargs)

   **绘图**

   .. py:method:: draw_line(x0, y0, x1, y1, color=..., thickness=1)
   .. py:method:: draw_rectangle(rect, color=..., thickness=1, fill=False)
   .. py:method:: draw_circle(circle, color=..., thickness=1, fill=False)
   .. py:method:: draw_cross(x, y, color=..., size=5, thickness=1)
   .. py:method:: draw_arrow(line, color=..., size=10, thickness=1)
   .. py:method:: draw_image(image, x=0, y=0, **kwargs)

   **滤波与形态学**

   .. py:method:: invert()
   .. py:method:: erode(ksize, *, threshold=0, mask=None)
   .. py:method:: dilate(ksize, *, threshold=0, mask=None)
   .. py:method:: blur(ksize, **kwargs)
   .. py:method:: gaussian(ksize, **kwargs)
   .. py:method:: median(ksize, **kwargs)
   .. py:method:: morph(ksize, kernel, **kwargs)
   .. py:method:: histeq(*, adaptive=False, clip_limit=-1, mask=None)

   **分析与特征检测**

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

   本页列出最常用的成员。完整方法集合，以及结果元组类型（``blob``、``line``、
   ``circle``、``rect``、``qrcode``、``barcode``、``apriltag``、``statistics``、
   ``histogram``）见 ``stubs/image.pyi``，后续将在此补全。
