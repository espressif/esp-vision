Introduce a New Model
=====================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION supports two model runtime paths: ESP-DL ``.espdl`` models use the :doc:`../api-reference/espdl` module, and TensorFlow Lite ``.tflite`` models use the :doc:`../api-reference/tflite` module. Model files are not built into the firmware; they live on board storage and are loaded at runtime. This guide adds a new model and runs it.

1. Obtain or Convert the Model
------------------------------

Choose the model runtime first:

- ESP-DL: get a ready ``.espdl`` from the `ESP-DL model zoo <https://github.com/espressif/esp-dl/tree/master/models>`_, or convert your own model to the ``.espdl`` format with the ESP-DL quantization/export toolchain, matching the selected chip (ESP32-P4, ESP32-S3, or ESP32-S31).
- TFLite Micro: use a TensorFlow Lite ``.tflite`` flatbuffer that fits TensorFlow Lite Micro and the enabled operator set. Quantized int8 models are usually the practical target on the current boards.

Keep the directory layout under ``models/`` in the repository when adding shared assets, mirroring ``models/espdet/`` and ``models/tflite/``.

2. Copy the Model to Board Storage
----------------------------------

Place the ``.espdl`` or ``.tflite`` file on storage the firmware can read, such as ``/sdcard`` or ``/flash``:

- SD card: copy the file onto the card, which mounts at ``/sdcard``.
- On-flash FAT (``ffat``): the data partition is exposed over USB MSC, so you can drag the file onto the mass-storage drive; it is visible as ``/flash``.

3. Pick the Right API
---------------------

Choose the API that matches the runtime and task:

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Task
     - API
     - Result
   * - ESP-DL object detection (ESPDet)
     - :py:class:`espdl.ESPDet`
     - ``(x, y, w, h, score, category)``
   * - ESP-DL object detection (YOLO11)
     - :py:class:`espdl.YOLO11`
     - ``(x, y, w, h, score, category)``
   * - ESP-DL pose detection
     - :py:class:`espdl.YOLO11nPose`
     - detection plus 17 COCO keypoints
   * - ESP-DL image classification
     - :py:class:`espdl.ImageNetCls`
     - ``(label, score)``
   * - Generic TFLite Micro execution
     - :py:class:`tflite.Model`
     - raw output tensors, or a callback result

For ESP-DL wrappers, pass ``mean``, ``std``, ``score``, ``nms``, ``topk``, or ``softmax`` to the constructor when the model needs different preprocessing or filtering. For TFLite Micro models, inspect ``input_shape``, ``input_dtype``, ``input_scale``, ``input_zero_point``, ``output_shape``, ``output_dtype``, ``output_scale``, and ``output_zero_point`` and implement the model-specific preprocessing or post-processing in Python or helper code.

4. Run ESP-DL Inference
-----------------------

.. code-block:: python

   import sensor, image, espdl

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   det = espdl.ESPDet("/sdcard/my_model.espdl", score=0.5, nms=0.45)

   while True:
       img = sensor.snapshot()
       for x, y, w, h, score, category in det.detect(img):
           img.draw_rectangle(x, y, w, h, color=(255, 0, 0))
       img.flush()

5. Run TFLite Micro Inference
-----------------------------

.. code-block:: python

   import tflite

   def fill_input(buffer, shape, dtype_code):
       # Fill model-specific quantized input bytes.
       ...

   model = tflite.Model("/sdcard/my_model.tflite")
   try:
       print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
       print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)
       outputs = model.predict([fill_input])
       raw = outputs[0]
   finally:
       model.deinit()

See ``example/03-Machine-Learning/00-ESP-DL/`` for ESP-DL scripts (``espdet_pico.py``, ``yolo11.py``, ``yolo11n_pose.py``, ``imagenet_cls.py``), and ``example/03-Machine-Learning/01-TFLite/`` for TFLite Micro scripts (``person_detection.py`` and ``sine.py``).

6. Optional: Profiling and Validation
-------------------------------------

Use :py:func:`espdl.load_model` with ``profile=True`` to emit ESP-DL profiling output when verifying a new ``.espdl`` model's performance. For TFLite Micro models, print ``model.len``, ``model.ram``, input metadata, and output metadata to verify flash size, arena size, tensor layout, and quantization before tuning post-processing.
