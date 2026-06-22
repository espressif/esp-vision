tflite -- 模型推理
==================

:link_to_translation:`en:[English]`

``tflite`` 模块加载 TensorFlow Lite ``.tflite`` flatbuffer，并通过 TensorFlow Lite Micro 运行。它为 TFLite 模型提供通用执行接口；当某类模型需要共享预处理或后处理时，也可以在其上继续封装更高层的任务接口。

基本用法
--------

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

``predict()`` 会以 ulab ndarray 返回原始输出 tensor。量化输出不会自动解码；模型需要时，应使用 ``output_scale`` 和 ``output_zero_point`` 自行换算。

Callable 输入
-------------

对于图像模型，直接填充输入 tensor 往往比先构造中间 ndarray 更省：

.. code-block:: python

   def fill_input(buffer, shape, dtype_code):
       # 按模型要求填充量化后的原始字节。
       ...

   outputs = model.predict([fill_input])

callable 会收到输入 tensor 的可写 bytearray 视图、tensor shape，以及 ``ord("b")`` 这类整数 dtype code。这个路径适合在 Python 示例中处理图像缩放、灰度转换和量化。

后处理
------

``Model`` 可以接收默认 ``postprocess`` 回调，``predict()`` 也可以接收单次 ``callback``。回调参数为 ``(model, inputs, raw_outputs)``，并且可以返回任意 Python 对象。

.. seealso::

   可运行示例：``example/03-Machine-Learning/01-TFLite``\ （person/no-person 与 sine smoke test）。

.. include:: _generated/tflite.rst
