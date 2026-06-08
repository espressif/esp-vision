espdl -- Model Inference
========================

:link_to_translation:`zh_CN:[中文]`

.. py:module:: espdl

The ``espdl`` module runs ESP-DL ``.espdl`` models on captured images. It
provides task-specific wrappers for object detection, pose detection, and image
classification. The source of truth for this page is ``stubs/espdl.pyi``. To
deploy a new model, see :doc:`../how-to/add-model`.

Example
-------

.. code-block:: python

   import sensor, espdl

   det = espdl.ESPDet("/sdcard/espdet_pico.espdl")
   img = sensor.snapshot()
   for x, y, w, h, score, category in det.detect(img):
       img.draw_rectangle(x, y, w, h)

Functions
---------

.. py:function:: load_model(path, *, profile=False)

   Preload an ESP-DL model file.

   :param str path: model path, e.g. ``/sdcard/model.espdl`` or
                    ``/flash/model.espdl``.
   :param bool profile: ``True`` enables ESP-DL profiling output when supported.
   :rtype: bool

Detection Result Tuples
-----------------------

- Detection: ``(x, y, w, h, score, category)``
- Pose: ``(x, y, w, h, score, category, keypoints)`` with 17 COCO keypoints
- Classification: ``(label, score)``

Classes
-------

.. py:class:: ESPDet(path, *, score=None, nms=None, mean=None, std=None)

   ESPDet object detector.

   .. py:method:: detect(image)

      Run object detection. ``image`` is an RGB565 or grayscale image; returns a
      list of detection tuples.

   .. py:method:: set_thresholds(*, score=None, nms=None)

      Update confidence / NMS thresholds; ``None`` keeps the current value.

   .. py:method:: deinit()

      Release model resources.

.. py:class:: YOLO11(path, *, score=None, nms=None, topk=10, mean=None, std=None)

   YOLO11 object detector. Same ``detect`` / ``set_thresholds`` / ``deinit``
   surface as :py:class:`ESPDet`, with a ``topk`` cap on detections per frame.

.. py:class:: YOLO11nPose(path, *, score=None, nms=None, topk=10, mean=None, std=None)

   YOLO11n COCO pose detector. ``detect`` returns pose tuples with 17 keypoints
   per person.

.. py:class:: ImageNetCls(path, *, topk=5, score=None, mean=None, std=None, softmax=True)

   ImageNet classifier.

   .. py:method:: classify(image)

      Run classification; returns a list of ``(label, score)`` tuples.

   .. py:method:: set_thresholds(*, topk=None, score=None)
   .. py:method:: deinit()
