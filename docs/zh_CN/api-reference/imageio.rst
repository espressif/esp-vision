image.ImageIO -- 图像流
=======================

:link_to_translation:`en:[English]`

``image.ImageIO`` 类型用于录制和回放图像序列，并保留帧间时间间隔。文件流在存储上
读写容器文件；内存流则把帧保存在预分配的缓冲区中。它以 ``image.ImageIO`` 的形式
暴露在 ``image`` 模块上；类型存根位于 ``stubs/imageio.pyi``\ 。

.. code-block:: python

   import sensor, image

   stream = image.ImageIO("/sdcard/stream.bin", "w")
   for _ in range(30):
       stream.write(sensor.snapshot())
   stream.close()

.. seealso::

   :doc:`../concepts/codec-streaming` 一并介绍了 ImageIO、JPEG、H.264 以及
   USB CDC 预览通路。

.. include:: _generated/imageio.rst
