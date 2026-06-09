image -- 图像处理
=================

:link_to_translation:`en:[English]`

``image`` 模块提供 :py:class:`Image` 对象以及基于 OpenMV ``imlib`` 的视觉算法：
绘图、格式转换、滤波、颜色/色块分析，以及特征检测（直线、圆、矩形、二维码、
条形码、AprilTag）。编解码流类型 :py:class:`imageio.ImageIO` 见 :doc:`imageio`\ 。

.. code-block:: python

   import sensor, image

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   img = sensor.snapshot()
   img.draw_rectangle(10, 10, 50, 50, color=(255, 0, 0))
   for blob in img.find_blobs([(30, 100, 15, 127, 15, 127)]):
       img.draw_cross(blob.cx(), blob.cy())

大多数原地操作的方法会返回图像本身，因此可以链式调用：
``img.to_grayscale().gaussian(1).binary([(128, 255)])``\ 。

.. seealso::

   关于这些方法背后的原理，参见 :doc:`../concepts/image-model`\ （像素格式、
   色彩空间、帧缓冲）与 :doc:`../concepts/image-processing`\ （滤波、阈值化、
   特征检测）。

.. include:: _generated/image.rst
