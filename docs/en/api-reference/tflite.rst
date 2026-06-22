tflite -- Model Inference
=========================

:link_to_translation:`zh_CN:[中文]`

The ``tflite`` module loads TensorFlow Lite ``.tflite`` flatbuffers and runs them through TensorFlow Lite Micro. It provides a general model execution interface for TFLite models, while higher-level task helpers can be layered on top when a model family needs shared preprocessing or post-processing.

Basic Usage
-----------

.. code-block:: python

   import tflite

   model = tflite.Model("/sdcard/sine.tflite")
   try:
       print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
       print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)
       outputs = model.predict([input_array])
       raw = outputs[0]
   finally:
       model.deinit()

``predict()`` returns raw output tensors as ulab ndarrays. Quantized outputs are not automatically decoded; use ``output_scale`` and ``output_zero_point`` to convert values when the model requires it.

Callable Inputs
---------------

For image models it is often cheaper to fill the input tensor directly instead of building an intermediate ndarray:

.. code-block:: python

   def fill_input(buffer, shape, dtype_code):
       # Fill buffer with model-specific quantized bytes.
       ...

   outputs = model.predict([fill_input])

The callable receives a writable bytearray view of the raw input tensor, the tensor shape, and an integer dtype code such as ``ord("b")`` for int8. This path is useful for image resizing, grayscale conversion, and quantization in Python examples.

Post-processing
---------------

``Model`` can take a default ``postprocess`` callback, and ``predict()`` can take a one-shot ``callback``. The callback receives ``(model, inputs, raw_outputs)`` and can return any Python object.

.. seealso::

   Runnable examples: ``example/03-Machine-Learning/01-TFLite`` (person/no-person and sine smoke tests).

.. include:: _generated/tflite.rst
