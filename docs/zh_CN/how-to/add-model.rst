引入新的模型
============

:link_to_translation:`en:[English]`

ESP-VISION 通过 :doc:`../api-reference/espdl` 模块运行 ESP-DL ``.espdl`` 模型。模型不会
编入固件，而是存放在板级存储中、运行时加载。本指南介绍如何添加并运行一个新模型。

1. 获取或转换模型
-----------------

可从 `ESP-DL 模型库 <https://github.com/espressif/esp-dl/tree/master/models>`_
获取现成的 ``.espdl``，或使用 ESP-DL 的量化/导出工具链将自有模型转换为 ``.espdl`` 格式，
并与目标芯片（ESP32-P4、ESP32-S3 或 ESP32-S31）匹配。

向仓库添加共享资源时，请保持 ``models/`` 下的目录结构，参照 ``models/espdet/``。

2. 将模型拷贝到板级存储
-----------------------

将 ``.espdl`` 文件放到固件可读取的存储中，例如 ``/sdcard`` 或 ``/flash``：

- SD 卡：将文件拷贝到卡上，挂载点为 ``/sdcard``。
- 片上 FAT（``ffat``）：数据分区通过 USB MSC 暴露，可将文件拖入大容量存储盘，路径为
  ``/flash``。

3. 选择合适的封装类
-------------------

根据模型任务选择对应的 ``espdl`` 封装：

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - 任务
     - 类
     - 结果
   * - 目标检测（ESPDet）
     - :py:class:`espdl.ESPDet`
     - ``(x, y, w, h, score, category)``
   * - 目标检测（YOLO11）
     - :py:class:`espdl.YOLO11`
     - ``(x, y, w, h, score, category)``
   * - 姿态检测
     - :py:class:`espdl.YOLO11nPose`
     - 检测结果加 17 个 COCO 关键点
   * - 图像分类
     - :py:class:`espdl.ImageNetCls`
     - ``(label, score)``

若模型需要不同的预处理，可向构造函数传入 ``mean``、``std``、``score``、``nms``、
``topk`` 或 ``softmax``。

4. 运行推理
-----------

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

可运行脚本见 ``example/03-Machine-Learning/00-ESP-DL/``（``espdet_pico.py``、
``yolo11.py``、``yolo11n_pose.py``、``imagenet_cls.py``）。

5. 可选：性能分析
-----------------

在验证新模型性能时，可使用 :py:func:`espdl.load_model` 并设置 ``profile=True``，以输出
ESP-DL 的性能分析信息。
