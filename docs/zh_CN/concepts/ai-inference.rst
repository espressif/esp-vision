AI 推理
=======

:link_to_translation:`en:[English]`

ESP-VISION 通过 :doc:`../api-reference/espdl` 和 :doc:`../api-reference/tflite` 提供端侧神经网络执行能力：前者使用 `ESP-DL <https://github.com/espressif/esp-dl>`_\ ，后者通过 TensorFlow Lite Micro 运行 TensorFlow Lite ``.tflite`` 模型。可用 API 覆盖从面向任务的辅助接口到通用模型执行；模型相关的预处理或后处理可以放在模块绑定、辅助代码或 Python 脚本中，取决于具体模型族。

模型运行时
----------

ESP-DL 和 TFLite Micro 是 ESP-VISION 当前暴露的两条模型运行路径。``espdl`` 使用 ``.espdl`` 文件，``tflite.Model`` 使用 ``.tflite`` 文件。模型文件可放在 ``/sdcard`` 或 ``/flash`` 等板级存储中，并在运行时加载。具体 API 形态、量化元数据、输入输出解释由所选运行时和模型共同定义。

模型文件与量化
--------------

模型通常在 PC 端从训练框架导出或转换后，再拷贝到板级存储。对于微控制器，量化模型通常更合适，因为它能降低模型体积、激活内存和计算开销。量化会用 scale 与 zero point 等元数据把浮点 tensor 范围映射到整数值；具体元数据和文件布局取决于运行时及转换工具链。

推理流程
--------

一次推理调用通常组合若干阶段：准备输入数据、运行量化网络，再把原始输出解码成应用可用的结果：

.. seqdiag::

   seqdiag {
     "脚本"; "API"; "运行时";

     "脚本" -> "API" [label = "图像或 tensor 输入"];
     "API" -> "API" [label = "准备输入"];
     "API" -> "运行时" [label = "模型输入"];
     "运行时" -> "运行时" [label = "推理"];
     "运行时" --> "API" [label = "原始 tensor"];
     "API" -> "API" [label = "可选解码"];
     "API" --> "脚本" [label = "结果对象"];
   }

预处理
------

推理前，输入数据必须与模型训练时一致：

- **形状** 必须匹配模型输入 tensor，图像模型还包括分辨率和通道数。
- **颜色** 顺序与像素格式必须匹配训练流程。
- **归一化或量化** 必须使用模型期望的 scale、zero point、mean、standard deviation 或其他变换。部分 API 会把这些暴露为参数，示例也可能直接在 Python 中展示。

后处理
------

网络原始输出通常需要按任务解码后，才能被应用直接使用：

- **目标检测**\ （:py:class:`espdl.ESPDet`\ 、:py:class:`espdl.YOLO11`\ ）产生带类别 分数的候选框。置信度 ``score`` 阈值剔除弱框，**非极大值抑制**\ （``nms``\ ）去除 重叠重复，最终得到 ``(x, y, w, h, score, category)`` 元组。``YOLO11`` 还用 ``topk`` 限制数量。
- **姿态估计**\ （:py:class:`espdl.YOLO11nPose`\ ）为每个检测附加 17 个 COCO 关键点。
- **分类**\ （:py:class:`espdl.ImageNetCls`\ ）应用可选的 ``softmax``\ ，返回 ``topk`` 个 ``(label, score)`` 对。

阈值和结果数量通常可以在运行时调整而无需重新加载模型，便于适应光照、距离或场景密度变化。

内存与性能
----------

模型及其激活缓冲较大，分配在 PSRAM 中。请加载一次模型并跨帧复用，而不要逐帧 重建。用完后调用 ``deinit()``\ （或释放最后一个引用）以释放模型。推理开销通常随输入分辨率和模型规模增长，因此较小模型一般比全尺寸网络帧率更高。
