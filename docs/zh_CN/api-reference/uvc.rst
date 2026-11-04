uvc — USB 摄像头输出
====================

:link_to_translation:`en:[English]`

启用 USB-OTG Device 的板卡启动时同时提供 CDC、MSC 和 UVC。PC 摄像头软件打开 ESP-VISION Camera 即可接收图像。默认输出固件内置 ESP-VISION logo，不初始化或修改 ``sensor``。禁用 USB Device 的板卡不提供此模块。

首版支持 MJPEG、320×240、10 FPS，使用等时传输，JPEG 帧上限为 48 KiB。实际帧率受主机调度和其他 USB 流量影响。``get_mode()`` 返回支持的模式；``is_streaming()`` 仅表示主机已启用流接口，不能证明某个软件已显示图像。

.. py:module:: uvc

.. py:function:: write(img)

   发布处理后的 QVGA ``image.Image``。主机未开启输出时返回 ``False``；帧已复制到固件持有的缓冲区时返回 ``True``，不代表主机已收到。RGB565、灰度和 JPEG 输入通过共享 JPEG 编码层处理。尺寸错误或编码结果超限抛出 ``ValueError``，编码或分配失败抛出 ``OSError``。返回后可以立即复用相机缓冲区。待发送帧可被新帧替换，正在发送的帧不会被覆盖。

.. py:function:: set_frame(img)

   复制固定 QVGA 图像，无需主机已打开。解除 preview 绑定，持续重复该帧，直到替换或 reset。成功返回 ``True``，失败时保留旧图源。

.. py:function:: bind(source)

   ``"preview"`` 绑定到 ``img.flush()`` 发布的处理结果。UVC 和 EV-MUX 共用一次 JPEG 编码，并独立判断传输就绪状态；无需 IDE 会话即可使用 UVC。其他尺寸的帧不送往 UVC，仍可通过 EV-MUX 预览。帧超限或 UVC 分配失败时丢帧，不影响 EV-MUX。``None`` 解除绑定并恢复 logo。新绑定在收到匹配帧前也显示 logo。

.. py:function:: reset()

   解除图源并恢复 logo，不重置 USB 控制器，不断开 CDC/MSC。VM 软复位也恢复 logo。实时生产者暂停或停止发布时，默认重复最后一帧。

.. py:function:: is_streaming()

   返回主机是否已启用流接口且 USB 总线未挂起。

.. py:function:: get_mode()

   返回 ``{"format": "MJPEG", "width": 320, "height": 240, "interval_us": 100000}``，仅公布并接受这一模式。

相机处理结果可以同时送往两种输出：

.. code-block:: python

   import sensor
   import uvc

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   uvc.bind("preview")
   try:
       while True:
           img = sensor.snapshot()
           img.draw_rectangle((20, 20, 100, 80), color=(255, 0, 0))
           img.flush()
   finally:
       uvc.reset()

无需 EV-MUX 预览时，也可直接使用 ``uvc.write(img)``。同一帧不要同时走两条发布路径。USB 回调内不执行 Python 回调或相机抓帧。
