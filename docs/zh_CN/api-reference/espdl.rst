espdl -- 模型推理
=================

:link_to_translation:`en:[English]`

``espdl`` 模块在采集到的图像上运行 ESP-DL 的 ``.espdl`` 模型，并为常见任务提供了
封装：目标检测（:py:class:`ESPDet`\ 、:py:class:`YOLO11`\ ）、姿态估计
（:py:class:`YOLO11nPose`\ ）和图像分类（:py:class:`ImageNetCls`\ ）。

.. code-block:: python

   import sensor, espdl

   det = espdl.ESPDet("/sdcard/espdet_pico.espdl")
   img = sensor.snapshot()
   for x, y, w, h, score, category in det.detect(img):
       img.draw_rectangle(x, y, w, h)

结果元组
--------

- 检测：``(x, y, w, h, score, category)``
- 姿态：``(x, y, w, h, score, category, keypoints)``\ ，含 17 个 COCO 关键点
- 分类：``(label, score)``

.. seealso::

   :doc:`../concepts/ai-inference` 介绍 ESP-DL 推理流程、``.espdl`` 格式、量化以及
   前后处理。部署新模型请参见 :doc:`../how-to/add-model`\ 。

.. include:: _generated/espdl.rst
