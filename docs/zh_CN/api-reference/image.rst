image -- 图像处理
=================

:link_to_translation:`en:[English]`

``image`` 模块提供 :py:class:`Image` 对象以及基于 OpenMV ``imlib`` 的视觉算法： 绘图、格式转换、滤波、颜色/色块分析，以及特征检测（直线、线段、圆、矩形、二维码、 AprilTag）。编解码流类型 :py:class:`imageio.ImageIO` 见 :doc:`imageio`\ 。

.. only:: esp32p4

   当前 ESP32-P4 板级配置还会启用 ZXing-C++ 条形码后端和 :py:meth:`image.Image.find_barcodes`\ 。

绘图与颜色色块追踪
------------------

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

RGB565 阈值包含六个 LAB 范围值：``(L_min, L_max, A_min, A_max, B_min, B_max)``。``pixels_threshold`` 会过滤匹配像素过少的区域，``area_threshold`` 会过滤外接矩形面积过小的区域。色块坐标以源图像像素为单位，可直接传给绘图方法。

滤波与二值分割
--------------

.. code-block:: python

   sensor.set_pixformat(sensor.GRAYSCALE)
   img = sensor.snapshot()
   img.gaussian(1)
   img.binary([(80, 255)])
   img.erode(1)
   img.dilate(1)
   img.flush()

``gaussian()`` 在阈值处理前抑制高频噪声。``binary()`` 将匹配的灰度值转换为有效二值，随后先腐蚀再膨胀可去除孤立像素并恢复主体区域。这些方法会原地修改图像。

二维码识别
----------

.. code-block:: python

   sensor.set_pixformat(sensor.GRAYSCALE)
   img = sensor.snapshot()
   for code in img.find_qrcodes():
       img.draw_rectangle(code.rect(), color=255, thickness=2)
       print(code.payload())

``find_qrcodes()`` 返回二维码的几何信息和解码内容。灰度输入通常可以降低处理开销；如果二维码只会出现在画面的固定区域，还可以指定较小的 ROI。

直线、线段与圆检测
------------------

.. code-block:: python

   img = sensor.snapshot()
   for line in img.find_lines(threshold=1400, theta_margin=25, rho_margin=25):
       img.draw_line(line.line(), color=255, thickness=2)

   for segment in img.find_line_segments(threshold=50, merge_distance=10):
       img.draw_line(segment.line(), color=255, thickness=2)

   for circle in img.find_circles(threshold=2500, r_min=8, r_max=80, r_step=4):
       img.draw_circle(circle.x(), circle.y(), circle.r(), color=255, thickness=2)

``find_lines()`` 和 ``find_circles()`` 使用霍夫累加器阈值。``find_line_segments()`` 使用 Edge Drawing Lines 检测器，其中 ``threshold`` 是用于产生并追踪边缘的最小 Sobel 梯度幅值，``merge_distance`` 用于合并邻近线段。提高阈值可减少较弱的检测结果；限制半径范围、ROI 或图像分辨率可以降低计算量，并通常能够提升稳定性。

编码并保存图像
--------------

.. code-block:: python

   import image

   img = sensor.snapshot()
   jpg = img.to_jpeg(quality=85, subsampling=image.JPEG_SUBSAMPLING_420)
   jpg.save("/sdcard/snapshot.jpg")
   print("encoded bytes:", jpg.size())

JPEG quality 用于权衡编码体积与图像细节，4:2:0 色度抽样通常可以得到更小的彩色图像。保存图像前，目标文件系统必须已经挂载且可写。

大多数原地操作的方法会返回图像本身，因此可以链式调用： ``img.to_grayscale().gaussian(1).binary([(128, 255)])``\ 。

.. seealso::

   关于这些方法背后的原理，参见 :doc:`../concepts/image-model`\ （像素格式、 色彩空间、帧缓冲）与 :doc:`../concepts/image-processing`\ （滤波、阈值化、 特征检测）。

   可运行示例：``example/02-Image-Processing``\ （绘图、滤波、颜色追踪、帧差分）、``example/04-Barcodes``\ （二维码）与 ``example/05-Feature-Detection``\ （AprilTag、直线、线段、圆）。

.. include:: _generated/image.rst
