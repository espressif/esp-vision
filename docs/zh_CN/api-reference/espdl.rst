espdl -- 模型推理
=================

:link_to_translation:`en:[English]`

.. py:module:: espdl

``espdl`` 模块在采集到的图像上运行 ESP-DL ``.espdl`` 模型，提供针对目标检测、姿态检测与
图像分类的专用封装。本页内容以 ``stubs/espdl.pyi`` 为准。部署新模型请参阅
:doc:`../how-to/add-model`。

示例
----

.. code-block:: python

   import sensor, espdl

   det = espdl.ESPDet("/sdcard/espdet_pico.espdl")
   img = sensor.snapshot()
   for x, y, w, h, score, category in det.detect(img):
       img.draw_rectangle(x, y, w, h)

函数
----

.. py:function:: load_model(path, *, profile=False)

   预加载一个 ESP-DL 模型文件。

   :param str path: 模型路径，例如 ``/sdcard/model.espdl`` 或 ``/flash/model.espdl``。
   :param bool profile: 当支持时，``True`` 启用 ESP-DL 性能分析输出。
   :rtype: bool

检测结果元组
------------

- 检测：``(x, y, w, h, score, category)``
- 姿态：``(x, y, w, h, score, category, keypoints)``，含 17 个 COCO 关键点
- 分类：``(label, score)``

类
--

.. py:class:: ESPDet(path, *, score=None, nms=None, mean=None, std=None)

   ESPDet 目标检测器。

   .. py:method:: detect(image)

      运行目标检测。``image`` 为 RGB565 或灰度图，返回检测元组列表。

   .. py:method:: set_thresholds(*, score=None, nms=None)

      更新置信度 / NMS 阈值；``None`` 表示保持当前值。

   .. py:method:: deinit()

      释放模型资源。

.. py:class:: YOLO11(path, *, score=None, nms=None, topk=10, mean=None, std=None)

   YOLO11 目标检测器。与 :py:class:`ESPDet` 具有相同的 ``detect`` /
   ``set_thresholds`` / ``deinit`` 接口，并通过 ``topk`` 限制每帧检测数量。

.. py:class:: YOLO11nPose(path, *, score=None, nms=None, topk=10, mean=None, std=None)

   YOLO11n COCO 姿态检测器。``detect`` 返回包含每个人 17 个关键点的姿态元组。

.. py:class:: ImageNetCls(path, *, topk=5, score=None, mean=None, std=None, softmax=True)

   ImageNet 分类器。

   .. py:method:: classify(image)

      运行分类；返回 ``(label, score)`` 元组列表。

   .. py:method:: set_thresholds(*, topk=None, score=None)
   .. py:method:: deinit()
