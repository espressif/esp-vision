AI Inference
============

:link_to_translation:`zh_CN:[中文]`

The :doc:`../api-reference/espdl` module runs neural networks on the device
using `ESP-DL <https://github.com/espressif/esp-dl>`_, Espressif's deep-learning
inference library. ESP-VISION wraps ESP-DL in task-specific classes so a script
can go from an image to detections, poses, or class labels in a few lines, while
ESP-DL handles the heavy tensor math on the chip's vector unit (and, on
ESP32-P4, additional acceleration).

The ``.espdl`` model format
---------------------------

Models are distributed as ``.espdl`` files. An ``.espdl`` bundle contains the
network graph, the trained weights, and -- crucially -- **quantization**
information. A model is converted from a floating-point framework (PyTorch,
TensorFlow) into ESP-DL format ahead of time; see :doc:`../how-to/add-model` for
the deployment workflow. Models live on storage (``/sdcard/...``) or the flash
data partition (``/flash/...``) and are loaded with the wrapper constructor or
:py:func:`espdl.load_model`.

Quantization
------------

Microcontrollers have no FPU budget for full 32-bit float inference at frame
rate, so ESP-DL runs **quantized** models, typically 8-bit (and 16-bit where
accuracy demands it) integer arithmetic. Quantization maps the float range of
each tensor onto integers with a scale factor, shrinking the model and letting
the hardware multiply-accumulate integers far faster. The accuracy cost is
usually small, but it is the reason a model must be quantized during conversion
rather than loaded directly from its training checkpoint.

Inference flow
--------------

A single ``detect()`` (or ``classify()``) call hides several stages. The wrapper
prepares the input, ESP-DL runs the quantized network, and the wrapper decodes
the raw output into Python tuples:

.. seqdiag::

   seqdiag {
     "script"; "wrapper"; "ESP-DL";

     "script" -> "wrapper" [label = "detect(img)"];
     "wrapper" -> "wrapper" [label = "resize + normalize"];
     "wrapper" -> "ESP-DL" [label = "quantized input"];
     "ESP-DL" -> "ESP-DL" [label = "int8 inference"];
     "ESP-DL" --> "wrapper" [label = "raw tensors"];
     "wrapper" -> "wrapper" [label = "decode + NMS + topk"];
     "wrapper" --> "script" [label = "list of tuples"];
   }

Pre-processing
--------------

Before inference the input image must match what the model was trained on:

- **Resize** to the model's input resolution. The wrappers handle this, scaling
  the captured frame to the network input size.
- **Color** order and channel count must match (most detectors expect RGB).
- **Normalization** subtracts a per-channel ``mean`` and divides by a per-channel
  ``std``. These are constructor keywords (``mean=``/``std=``) so you can match
  the exact preprocessing the model expects; the defaults suit the bundled
  models.

Post-processing
---------------

The raw network outputs are decoded into friendly Python tuples:

- **Object detection** (:py:class:`espdl.ESPDet`, :py:class:`espdl.YOLO11`)
  produces candidate boxes with class scores. A confidence ``score`` threshold
  drops weak boxes and **non-maximum suppression** (``nms``) removes overlapping
  duplicates, leaving ``(x, y, w, h, score, category)`` tuples. ``YOLO11`` also
  caps results with ``topk``.
- **Pose estimation** (:py:class:`espdl.YOLO11nPose`) adds 17 COCO keypoints per
  detection.
- **Classification** (:py:class:`espdl.ImageNetCls`) applies an optional
  ``softmax`` and returns the ``topk`` ``(label, score)`` pairs.

Thresholds can be tuned at runtime with ``set_thresholds`` without reloading the
model, which is handy when adapting to lighting or distance.

Memory and performance
----------------------

Models and their activation buffers are large and are allocated in PSRAM. Load a
model once and reuse the wrapper across frames rather than recreating it per
frame. Call ``deinit()`` (or drop the last reference) to free a model when you
are done. Detection cost scales with input resolution, so smaller models such as
``espdet_pico`` run at higher frame rates than full-size networks.
