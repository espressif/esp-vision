摄像头流水线
============

:link_to_translation:`en:[English]`

:py:func:`sensor.snapshot` 看起来只是一次调用，但其背后是一条与硬件相关的采集流水线，
把光子变成 :py:class:`image.Image`\ 。了解这些阶段有助于理解配置调用
（``set_pixformat``\ 、``set_framesize``\ 、``skip_frames``\ ）以及各开发板的性能特征。

从传感器到图像
--------------

#. **配置。** :py:func:`sensor.reset` 通过控制总线启动图像传感器并应用板级默认配置；
   :py:func:`sensor.set_pixformat` 与 :py:func:`sensor.set_framesize` 选择输出格式与
   分辨率。
#. **取流。** 传感器经摄像头接口（MIPI-CSI 或 DVP）把像素通过 DMA 送入 PSRAM 中的
   帧缓冲。采集采用双缓冲：处理当前帧的同时填充下一帧。
#. **转换。** 原始传感器数据（通常是 Bayer 或 YUV）被转换为请求的像素格式。在
   ESP32-P4 上由硬件 **PPA**\ （像素处理加速器）完成缩放与色彩转换；在 ESP32-S3 上
   则由软件完成。
#. **稳定。** 自动曝光与自动白平衡需要若干帧才能收敛，因此示例会在配置后调用
   ``sensor.skip_frames(time=2000)``\ 。
#. **快照。** :py:func:`sensor.snapshot` 把最新帧作为共享帧缓冲内存的
   :py:class:`image.Image` 返回（见 :doc:`image-model`\ ）。

各开发板后端
------------

摄像头后端在构建时按板选择：

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - 开发板系列
     - 后端
     - 说明
   * - ESP32-P4
     - ``esp_video`` / V4L2 + PPA
     - MIPI-CSI 传感器；硬件加速的缩放与色彩转换。
   * - ESP32-S3
     - ``esp32-camera``
     - DVP 传感器；软件转换，建议使用较小分辨率。

由于公开的 ``sensor`` API 在各板上完全一致，同一脚本可在两者上运行；区别仅在于可达到
的分辨率与帧率。

分辨率
------

``set_framesize`` 接受来自共享分辨率表的命名尺寸（``QQVGA``\ 、``QVGA``\ 、``VGA``
等）。较小的帧在每个阶段都更省内存、更省处理时间，因此当模型或算法只需小输入时，
应直接采集小帧，而不是采集大帧再缩小。
