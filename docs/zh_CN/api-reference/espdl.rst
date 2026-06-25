espdl -- 模型推理
=================

:link_to_translation:`en:[English]`

``espdl`` 模块在采集到的图像上运行 ESP-DL 的 ``.espdl`` 模型，并提供常见任务封装：目标检测（:py:class:`ESPDet`\ 、:py:class:`YOLO11`\ ）、姿态估计（:py:class:`YOLO11nPose`\ ）和图像分类（:py:class:`ImageNetCls`\ ），同时可通过 :py:class:`Model` 将 ESP-DL 原始输出 tensor 暴露给 Python。

原始输出 tensor
---------------

.. code-block:: python

   import sensor, espdl

   model = espdl.Model("/sdcard/custom.espdl", mean=(0, 0, 0), std=(255, 255, 255), letterbox=True)
   try:
       print("inputs:", model.inputs())
       print("outputs:", model.outputs())
       img = sensor.snapshot()
       outputs = model.predict(img)
       for name, tensor in outputs.items():
           _, shape, dtype, exponent, raw = tensor
           print(name, shape, dtype, exponent, len(raw))
   finally:
       model.deinit()

当内置任务封装不匹配模型输出布局时，可使用 :py:class:`Model`\ 。ESP-DL 仍负责图像预处理和推理；``predict()`` 返回原始输出 tensor，Python 代码再执行模型相关解码。``inputs()`` 和 ``outputs()`` 返回 ``TensorInfo`` 元组 ``(name, shape, dtype, exponent)``。``predict()`` 返回按输出 tensor 名称索引的 ``RawTensor`` 元组 ``(name, shape, dtype, exponent, bytes)``。应根据 ``dtype`` 解包原始字节，应用 ESP-DL 的二次幂 ``exponent`` 量化比例，再执行 sigmoid、框解码、NMS、softmax 或 top-k 选择等逻辑。若启用 ``letterbox=True``，将坐标映射回源图像前需要先去除模型输入中的 padding。

目标检测
--------

.. code-block:: python

   import sensor, espdl

   det = espdl.ESPDet("/sdcard/espdet_pico_224_224_face.espdl", score=0.5, nms=0.7)
   try:
       img = sensor.snapshot()
       for x, y, w, h, score, category in det.detect(img):
           img.draw_rectangle(x, y, w, h, color=(255, 0, 0), thickness=2)
           img.draw_string(x, max(0, y - 12), "%.2f:%d" % (score, category))
   finally:
       det.deinit()

``score`` 用于过滤低置信度候选框，``nms`` 用于控制重叠检测框的抑制。检测坐标会映射回输入图像，可直接用于绘图。模型应加载一次并在多帧之间复用；``deinit()`` 会释放模型权重和中间缓冲区。

图像分类
--------

.. code-block:: python

   import espdl, image

   img = image.Image("/sdcard/cat.jpg").to_rgb565(copy=True)
   classifier = espdl.ImageNetCls(
       "/sdcard/imagenet_cls_mobilenetv2_s8_v1.espdl",
       topk=5,
       score=0.0,
   )
   try:
       for label, score in classifier.classify(img):
           print(label, "%.4f" % score)
   finally:
       classifier.deinit()

分类结果按照模型输出顺序返回最多 ``topk`` 个 ``(label, score)`` 元组。必要时应将文件图像转换为 RGB565，确保输入格式可被封装层接受。

姿态估计
--------

.. code-block:: python

   pose = espdl.YOLO11nPose("/sdcard/espdet_yolo11n_pose_160_160_coco.espdl", score=0.35, topk=5)
   try:
       img = sensor.snapshot()
       for x, y, w, h, score, category, keypoints in pose.detect(img):
           img.draw_rectangle(x, y, w, h, color=(255, 0, 0))
           for px, py in keypoints:
               if px > 0 and py > 0:
                   img.draw_circle(px, py, 2, color=(0, 255, 0), fill=True)
   finally:
       pose.deinit()

每个姿态结果包含 17 个 COCO 关键点。缺失或低置信度关键点会返回为 ``(0, 0)``，绘图或计算关节几何关系前应将其跳过。

运行时调整阈值
--------------

.. code-block:: python

   det.set_thresholds(score=0.65, nms=0.6)

无需重新加载模型即可修改阈值，适合应用根据光照、距离或场景目标密度的变化进行动态调整。

结果元组
--------

- TensorInfo：``(name, shape, dtype, exponent)``
- RawTensor：``(name, shape, dtype, exponent, bytes)``
- 检测：``(x, y, w, h, score, category)``
- 姿态：``(x, y, w, h, score, category, keypoints)``\ ，含 17 个 COCO 关键点
- 分类：``(label, score)``

.. seealso::

   :doc:`../concepts/ai-inference` 介绍 ESP-DL 推理流程、``.espdl`` 格式、量化以及 前后处理。部署新模型请参见 :doc:`../how-to/add-model`\ 。

   可运行示例：``example/03-Machine-Learning/00-ESP-DL``\ （ESP-DL 原始输出解码、ESPDet、YOLO11、姿态、ImageNet 分类）。

.. include:: _generated/espdl.rst
