引入新的模型
============

:link_to_translation:`en:[English]`

ESP-VISION 支持两类模型运行路径：ESP-DL ``.espdl`` 模型使用 :doc:`../api-reference/espdl` 模块，TensorFlow Lite ``.tflite`` 模型使用 :doc:`../api-reference/tflite` 模块。模型不会编入固件，而是存放在板级存储中、运行时加载。本指南介绍如何添加并运行一个新模型。

1. 获取或转换模型
-----------------

先选择模型运行时：

- ESP-DL：可从 `ESP-DL 模型库 <https://github.com/espressif/esp-dl/tree/master/models>`_ 获取现成的 ``.espdl``，或使用 ESP-DL 的量化/导出工具链将自有模型转换为 ``.espdl`` 格式，并与目标芯片（ESP32-P4、ESP32-S3 或 ESP32-S31）匹配。
- TFLite Micro：使用 TensorFlow Lite ``.tflite`` flatbuffer，并确保模型适配 TensorFlow Lite Micro 以及固件启用的算子集合。在当前板级资源下，量化 int8 模型通常是更实际的目标。

向仓库添加共享资源时，请保持 ``models/`` 下的目录结构，参照 ``models/espdet/`` 和 ``models/tflite/``。

2. 将模型拷贝到板级存储
-----------------------

将 ``.espdl`` 或 ``.tflite`` 文件放到固件可读取的存储中，例如 ``/sdcard`` 或 ``/flash``：

- SD 卡：将文件拷贝到卡上，挂载点为 ``/sdcard``。
- 片上 FAT（``ffat``）：数据分区通过 USB MSC 暴露，可将文件拖入大容量存储盘，路径为 ``/flash``。

3. 选择合适的 API
-----------------

根据运行时和模型任务选择对应 API：

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - 任务
     - API
     - 结果
   * - ESP-DL 目标检测（ESPDet）
     - :py:class:`espdl.ESPDet`
     - ``(x, y, w, h, score, category)``
   * - ESP-DL 目标检测（YOLO11）
     - :py:class:`espdl.YOLO11`
     - ``(x, y, w, h, score, category)``
   * - ESP-DL 姿态检测
     - :py:class:`espdl.YOLO11nPose`
     - 检测结果加 17 个 COCO 关键点
   * - ESP-DL 图像分类
     - :py:class:`espdl.ImageNetCls`
     - ``(label, score)``
   * - ESP-DL 输出由 Python 解码
     - :py:class:`espdl.Model`
     - 带 tensor 元数据的原始输出字节
   * - 通用 TFLite Micro 执行
     - :py:class:`tflite.Model`
     - 原始输出 tensor，或回调返回值

对于 ESP-DL 封装，若模型需要不同的预处理或过滤参数，可向构造函数传入 ``mean``、``std``、``score``、``nms``、``topk`` 或 ``softmax``。当需要保留 ESP-DL 推理、但在 Python 中解码输出时，应检查 ``espdl.Model.inputs()`` 和 ``espdl.Model.outputs()``，再解码 ``predict()`` 返回的 ``RawTensor`` 字节。对于 TFLite Micro 模型，应检查 ``input_shape``、``input_dtype``、``input_scale``、``input_zero_point``、``output_shape``、``output_dtype``、``output_scale`` 和 ``output_zero_point``，并在 Python 或辅助代码中实现模型相关的预处理或后处理。

4. 运行 ESP-DL 推理
-------------------

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

5. 在 Python 中解码 ESP-DL 输出
-------------------------------

当模型可使用 ESP-DL 图像预处理和推理，但输出 tensor 需要由 Python 侧解码时，可使用 :py:class:`espdl.Model`\ 。

.. code-block:: python

   import sensor, espdl

   model = espdl.Model("/sdcard/my_model.espdl", mean=(0, 0, 0), std=(255, 255, 255), letterbox=True)
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

每个输出 tensor 都会以原始字节形式返回，并附带 ``shape``、``dtype`` 和 ESP-DL ``exponent`` 元数据。应按 tensor 类型解码字节、应用 exponent 比例，再执行 sigmoid、框解码、NMS、分类 top-k 或坐标去 letterbox 等模型相关后处理。

6. 运行 TFLite Micro 推理
-------------------------

.. code-block:: python

   import tflite

   def fill_input(buffer, shape, dtype_code):
       # 按模型要求填充量化后的输入字节。
       ...

   model = tflite.Model("/sdcard/my_model.tflite")
   try:
       print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
       print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)
       outputs = model.predict([fill_input])
       raw = outputs[0]
   finally:
       model.deinit()

ESP-DL 可运行脚本见 ``example/03-Machine-Learning/00-ESP-DL/``（``espdet_pico.py``、``espdet_pico_python.py``、``yolo11.py``、``yolo11n_pose.py``、``imagenet_cls.py``），TFLite Micro 可运行脚本见 ``example/03-Machine-Learning/01-TFLite/``（``person_detection.py`` 和 ``sine.py``）。

7. 可选：性能分析和验证
-----------------------

在验证新的 ``.espdl`` 模型性能时，可使用 :py:func:`espdl.load_model` 并设置 ``profile=True``，以输出 ESP-DL 的性能分析信息。对于 TFLite Micro 模型，可打印 ``model.len``、``model.ram``、输入元数据和输出元数据，先确认 flash 大小、arena 大小、tensor 布局和量化信息，再调试后处理。
