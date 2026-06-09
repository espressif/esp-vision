Introduce a New Model
=====================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION runs ESP-DL ``.espdl`` models through the :doc:`../api-reference/espdl`
module. Models are not built into the firmware; they live on board storage and
are loaded at runtime. This guide adds a new model and runs it.

1. Obtain or Convert the Model
------------------------------

Get a ready ``.espdl`` from the `ESP-DL model zoo
<https://github.com/espressif/esp-dl/tree/master/models>`_, or convert your own
model to the ``.espdl`` format with the ESP-DL quantization/export toolchain,
matching the target chip (ESP32-P4, ESP32-S3, or ESP32-S31).

Keep the directory layout under ``models/`` in the repository when adding shared
assets, mirroring ``models/espdet/``.

2. Copy the Model to Board Storage
----------------------------------

Place the ``.espdl`` file on storage the firmware can read, such as ``/sdcard``
or ``/flash``:

- SD card: copy the file onto the card, which mounts at ``/sdcard``.
- On-flash FAT (``ffat``): the data partition is exposed over USB MSC, so you
  can drag the file onto the mass-storage drive; it is visible as ``/flash``.

3. Pick the Right Wrapper
-------------------------

Choose the ``espdl`` wrapper that matches the model task:

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Task
     - Class
     - Result
   * - Object detection (ESPDet)
     - :py:class:`espdl.ESPDet`
     - ``(x, y, w, h, score, category)``
   * - Object detection (YOLO11)
     - :py:class:`espdl.YOLO11`
     - ``(x, y, w, h, score, category)``
   * - Pose detection
     - :py:class:`espdl.YOLO11nPose`
     - detection plus 17 COCO keypoints
   * - Image classification
     - :py:class:`espdl.ImageNetCls`
     - ``(label, score)``

If the model needs different preprocessing, pass ``mean``, ``std``, ``score``,
``nms``, ``topk``, or ``softmax`` to the constructor.

4. Run Inference
----------------

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

See ``example/03-Machine-Learning/00-ESP-DL/`` for runnable scripts
(``espdet_pico.py``, ``yolo11.py``, ``yolo11n_pose.py``, ``imagenet_cls.py``).

5. Optional: Profiling
----------------------

Use :py:func:`espdl.load_model` with ``profile=True`` to emit ESP-DL profiling
output when verifying a new model's performance.
