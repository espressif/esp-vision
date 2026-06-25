espdl -- Model Inference
========================

:link_to_translation:`zh_CN:[中文]`

The ``espdl`` module runs ESP-DL ``.espdl`` models on captured images. It provides task-specific wrappers for object detection (:py:class:`ESPDet`, :py:class:`YOLO11`), pose estimation (:py:class:`YOLO11nPose`), and image classification (:py:class:`ImageNetCls`), plus :py:class:`Model` for exposing raw ESP-DL output tensors to Python.

Raw Output Tensors
------------------

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

Use :py:class:`Model` when the built-in task wrappers do not match a model's output layout. ESP-DL still performs image preprocessing and inference; ``predict()`` returns the raw output tensors so Python code can run the model-specific decode. ``inputs()`` and ``outputs()`` return ``TensorInfo`` tuples ``(name, shape, dtype, exponent)``. ``predict()`` returns ``RawTensor`` tuples ``(name, shape, dtype, exponent, bytes)`` keyed by output tensor name. Decode the raw bytes according to ``dtype``, apply the ESP-DL power-of-two ``exponent`` scale, and run logic such as sigmoid, box decode, NMS, softmax, or top-k selection. When ``letterbox=True``, remove the model-input padding before mapping coordinates back to the source image.

Object Detection
----------------

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

``score`` removes low-confidence candidates, while ``nms`` controls suppression of overlapping boxes. Detection coordinates are mapped back to the input image and can be used directly for drawing. Load a model once and reuse it across frames; ``deinit()`` releases model weights and intermediate buffers.

Image Classification
--------------------

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

Classification returns up to ``topk`` ``(label, score)`` pairs ordered by the model output. Convert file images to RGB565 when necessary so the input format is accepted by the wrapper.

Pose Estimation
---------------

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

Each pose result contains 17 COCO keypoints. A missing or low-confidence point is returned as ``(0, 0)`` and should be skipped before drawing or computing joint geometry.

Adjust Thresholds at Runtime
----------------------------

.. code-block:: python

   det.set_thresholds(score=0.65, nms=0.6)

Thresholds can be changed without reloading the model, which is useful when adapting an application to lighting, distance, or scene-density changes.

Result tuples
-------------

- TensorInfo: ``(name, shape, dtype, exponent)``
- RawTensor: ``(name, shape, dtype, exponent, bytes)``
- Detection: ``(x, y, w, h, score, category)``
- Pose: ``(x, y, w, h, score, category, keypoints)`` with 17 COCO keypoints
- Classification: ``(label, score)``

.. seealso::

   :doc:`../concepts/ai-inference` describes the ESP-DL inference pipeline, the ``.espdl`` format, quantization, and pre-/post-processing. To deploy a new model, see :doc:`../how-to/add-model`.

   Runnable examples: ``example/03-Machine-Learning/00-ESP-DL`` (ESP-DL raw output decoding, ESPDet, YOLO11, pose, ImageNet classification).

.. include:: _generated/espdl.rst
