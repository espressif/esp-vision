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
     - 相机采集与串流示例，从 ``00-Snapshot`` 开始。
   * - ``02-Image-Processing``
     - 绘图、滤波、颜色追踪与帧差分。
   * - ``03-Machine-Learning``
     - ESP-DL 推理：ESPDet、YOLO11、YOLO11n 姿态、ImageNet 分类
       （``00-ESP-DL``）。
   * - ``04-Barcodes``
     - 二维码检测（``find_qrcodes.py``）。
   * - ``05-Feature-Detection``
     - AprilTag、直线与圆。
   * - ``06-Peripherals``
     - 存储（SD 卡）、显示/预览与 Wi-Fi（WebREPL）。

.. only:: esp32p4

   ESP32-P4 构建还支持 ``01-Camera/01-H264``、``01-Camera/02-RTSP``，以及
   ``05-Feature-Detection`` 下的一维条形码示例。

Wi-Fi MJPEG 串流
----------------

``01-Camera/03-MJPEG/wifi_mjpeg_stream.py`` 采集 JPEG 图像，并通过 HTTP MJPEG
串流输出。请先在脚本中设置 ``SSID`` 和 ``PASSWORD``，然后在固件提供
``network.WLAN`` 的相机开发板上运行，并用浏览器打开串口输出的
``http://<board-ip>/`` 地址。

该示例提供适合浏览器查看的 MJPEG 串流，并非 RTSP。可通过 ``JPEG_QUALITY`` 和
``FRAME_DELAY_MS`` 调整图像质量与帧间隔。实际可用性取决于开发板的相机、网络硬件
及固件配置。

与 API 参考的对应关系
---------------------

- ``01-Camera`` 与 ``06-Peripherals/01-Display`` 使用
  :doc:`../api-reference/sensor` 与 :doc:`../api-reference/display`。
- ``01-Camera/03-MJPEG`` 还使用标准 MicroPython ``network`` 和 ``socket`` 模块。
- ``02-Image-Processing``、``04-Barcodes`` 与 ``05-Feature-Detection`` 使用
  :doc:`../api-reference/image`。
- ``03-Machine-Learning`` 使用 :doc:`../api-reference/espdl`。

.. only:: esp32p4

   - ``01-Camera/01-H264`` 与 ``01-Camera/02-RTSP`` 使用
     :doc:`../api-reference/h264` 与 :doc:`../api-reference/rtsp`。
