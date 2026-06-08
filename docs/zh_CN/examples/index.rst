示例
====

:link_to_translation:`en:[English]`

ESP-VISION 在 ``example/`` 下提供可直接运行的 MicroPython 脚本。可将脚本拷贝到开发板，
或通过主机工具运行。各目录按主题组织。

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 目录
     - 内容
   * - ``00-HelloWorld``
     - 最简首个脚本（``helloworld.py``）。
   * - ``01-Camera``
     - 拍照、H.264 录制与 RTSP 推流
       （``00-Snapshot``、``01-H264``、``02-RTSP``）。
   * - ``02-Image-Processing``
     - 绘图、滤波、颜色追踪与帧差分。
   * - ``03-Machine-Learning``
     - ESP-DL 推理：ESPDet、YOLO11、YOLO11n 姿态、ImageNet 分类
       （``00-ESP-DL``）。
   * - ``04-Barcodes``
     - 二维码检测（``find_qrcodes.py``）。
   * - ``05-Feature-Detection``
     - 条形码、AprilTag、直线与圆。
   * - ``06-Peripherals``
     - 存储（SD 卡）、显示/预览与 Wi-Fi（WebREPL）。

与 API 参考的对应关系
---------------------

- ``01-Camera`` 与 ``06-Peripherals/01-Display`` 使用
  :doc:`../api-reference/sensor` 与 :doc:`../api-reference/display`。
- ``02-Image-Processing``、``04-Barcodes`` 与 ``05-Feature-Detection`` 使用
  :doc:`../api-reference/image`。
- ``03-Machine-Learning`` 使用 :doc:`../api-reference/espdl`。
- ``01-Camera/01-H264`` 与 ``01-Camera/02-RTSP`` 使用
  :doc:`../api-reference/h264` 与 :doc:`../api-reference/rtsp`。
