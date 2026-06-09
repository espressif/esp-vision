sensor -- 摄像头
================

:link_to_translation:`en:[English]`

``sensor`` 模块用于控制摄像头并采集图像帧。它的接口与 OpenMV 的 ``sensor``
保持一致，因此大多数 OpenMV 脚本无需改动即可移植。

典型流程是：复位传感器、选择像素格式与分辨率、等待自动曝光稳定，然后在循环中
采集图像。

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=2000)

   while True:
       img = sensor.snapshot()

.. seealso::

   :doc:`../concepts/camera-pipeline` 说明了一帧图像如何从图像传感器到达
   :py:class:`image.Image`\ ；:doc:`../concepts/image-model` 介绍像素格式与
   色彩空间。

.. include:: _generated/sensor.rst
