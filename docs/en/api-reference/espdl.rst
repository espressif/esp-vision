espdl -- Model Inference
========================

:link_to_translation:`zh_CN:[中文]`

The ``espdl`` module runs ESP-DL ``.espdl`` models on captured images. It
provides task-specific wrappers for object detection
(:py:class:`ESPDet`, :py:class:`YOLO11`), pose estimation
(:py:class:`YOLO11nPose`), and image classification (:py:class:`ImageNetCls`).

.. code-block:: python

   import sensor, espdl

   det = espdl.ESPDet("/sdcard/espdet_pico.espdl")
   img = sensor.snapshot()
   for x, y, w, h, score, category in det.detect(img):
       img.draw_rectangle(x, y, w, h)

Result tuples
-------------

- Detection: ``(x, y, w, h, score, category)``
- Pose: ``(x, y, w, h, score, category, keypoints)`` with 17 COCO keypoints
- Classification: ``(label, score)``

.. seealso::

   :doc:`../concepts/ai-inference` describes the ESP-DL inference pipeline,
   the ``.espdl`` format, quantization, and pre-/post-processing. To deploy a
   new model, see :doc:`../how-to/add-model`.

.. include:: _generated/espdl.rst
