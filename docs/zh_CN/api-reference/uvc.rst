uvc — USB 摄像头输出
====================

:link_to_translation:`en:[English]`

仅在构建中启用了原生 USB Device UVC 的板卡上，启动时才会同时提供 CDC、MSC 和 UVC。PC 摄像头软件打开 ESP-VISION Camera 即可接收图像。没有应用图像时，固件会按当前模式尺寸即时生成单色 ESP 标志 JPEG；打开 UVC 不会初始化或修改 ``sensor``。并非所有板卡都启用了此模块。

正式 fallback 模式只公布一种 MJPEG 格式：320×240、10 FPS，使用等时传输。JPEG 帧上限为 48 KiB。目标模式表需要在构建时选择开启，默认关闭；其第一项保留 320×240、10 FPS。已配置屏幕候选包括 P4X-EYE 和 S3-EYE 的 240×240，以及 P4X Function EV Board 的 1024×600。Mosaico 候选还包括 480×480 屏幕目标和 640×480 当前配置的 raw 输入目标，各自提供 10、20、30 FPS。P4 候选采用当前配置的 1280×720 raw 输入尺寸。P4X-VISION 和 AtomS3R-M12 的板级配置没有定义 LCD 尺寸，因此不添加屏幕候选。这些尺寸用于主机协商，可能与传感器的物理尺寸上限不同。所有 UVC 构建在运行时都接受尺寸不同于公布模式的输入图像，但不保证主机兼容这种尺寸不匹配的帧。实际传输也受主机调度和其他 USB 流量影响。``get_mode()`` 返回主机选择的模式；``is_streaming()`` 仅表示主机已启用流接口，不能证明某个软件已显示图像。

UVC 使用一个按需分配的 PSRAM 应用帧缓冲区；当前候选模式的上限为 48 KiB。成功调用后，UVC 已将 JPEG 复制到自己的缓冲区，应用可复用图像或编码器缓冲区。TinyUSB 发送该应用帧期间，不能覆盖它；传输完成或确认取消前，缓冲区继续由 UVC 持有。即时生成的 ESP 标志是单独的回退图源，不是第二个应用帧队列槽。

具备候选表的板卡需要在构建时同时设置 ``MICROPY_HW_USB_UVC_ENABLE_TARGET_MODES=1`` 和 ``MICROPY_HW_USB_UVC_MODE_TABLE_CAPABLE=1`` 才会启用实验模式。P4X-EYE 候选表包括 320×240、240×240 和 1280×720，每种分辨率提供 10、20、30 FPS。正式启用前，应结合实际固件、相机负载和 USB 连接验证主机兼容性。

.. py:module:: uvc

.. py:function:: write(img)

   发布处理后的 ``image.Image``。运行时接受尺寸不同于公布 UVC 模式的输入图像，但主机可能拒绝或错误处理尺寸不匹配的帧。主机 UVC 流未开启，或应用帧仍在传输时返回 ``False``；JPEG 已复制到 UVC 独立缓冲区时返回 ``True``，不代表主机已收到。RGB565、灰度和 JPEG 输入使用共享 JPEG 路径处理。无效输入或编码结果超限抛出 ``ValueError``，编码或分配失败抛出 ``OSError``。返回后可以立即复用相机缓冲区。尚未发送的帧可被新帧替换，正在发送的帧不会被覆盖。

.. py:function:: set_frame(img)

   仅在主机 UVC 流处于活动状态时复制固定图像。运行时接受尺寸不同于公布模式的输入图像，但不保证主机兼容。成功后解除 preview 绑定并持续重复该帧，直到替换或 reset。流未开启或应用缓冲区仍在传输时返回 ``False``。断流会清除固定帧，并在之后恢复即时生成的 ESP 标志。更新失败时保留旧图源。

.. py:function:: bind(source)

   ``"preview"`` 绑定到 ``img.flush()`` 发布的处理结果，无需 IDE 会话即可使用 UVC。运行时接受尺寸不同于公布模式的预览输入，但不保证主机兼容。帧超限或 UVC 分配失败时丢弃 UVC 帧，不阻塞 EV-MUX。``None`` 解除 preview 绑定；之后没有新应用帧时输出即时生成的 ESP 标志。

.. py:function:: reset()

   解除图源并在没有新帧时恢复即时生成的单色 ESP 标志；不重置 USB 控制器，不断开 CDC/MSC。VM 软复位也会清除图源。实时生产者暂停或停止发布时，默认重复最后一帧。

.. py:function:: is_streaming()

   返回主机是否已启用流接口且 USB 总线未挂起。

.. py:function:: get_mode()

   返回主机选择的模式。协商前，正式 fallback 和所有 opt-in 表的初始模式均为 ``{"format": "MJPEG", "width": 320, "height": 240, "interval_us": 100000}``。主机提交其他已公布模式后，返回值会显示对应尺寸和帧间隔。

.. py:function:: set_mode_callback(callback)

   注册可调用对象。主机每次成功 Commit 后，固件会以与 ``get_mode()`` 相同字段构成的字典通知该对象；如果注册前已有 Commit，也会安排一次最近模式通知。回调延后到 MicroPython VM 任务执行，不会在 USB 回调中执行。若 VM 调度前收到多个 Commit，只通知最新模式。回调异常由 MicroPython 输出，不会改变 USB Commit 结果。传入 ``None`` 解除注册；软复位会清除回调，``uvc.reset()`` 只更改图像来源。

应用可以将处理后的相机图像发布给预览输出：

.. code-block:: python

   import sensor
   import uvc

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   # QVGA 与正式发布的 320×240@10 fallback 模式匹配。
   uvc.bind("preview")
   try:
       while True:
           img = sensor.snapshot()
           img.draw_rectangle((20, 20, 100, 80), color=(255, 0, 0))
           img.flush()
   finally:
       uvc.reset()

无需 EV-MUX 预览时，也可直接使用 ``uvc.write(img)``。同一帧不要同时走两条发布路径。USB 回调内不执行 Python 回调或相机抓帧。

监听主机选择的模式：

.. code-block:: python

   import time
   import uvc

   def on_mode(mode):
       print("UVC mode:", mode["width"], mode["height"], mode["interval_us"])

   uvc.set_mode_callback(on_mode)
   try:
       while True:
           time.sleep_ms(1000)
   finally:
       uvc.set_mode_callback(None)

正式发布模式的兼容示例：

.. code-block:: python

   import sensor
   import time
   import uvc

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)  # 320×240，与 fallback 模式匹配。
   sensor.skip_frames(time=1000)
   while not uvc.is_streaming():
       time.sleep_ms(50)

   frame = sensor.snapshot()
   while not uvc.set_frame(frame):  # set_frame 需要主机流处于活动状态。
       time.sleep_ms(20)
   try:
       while uvc.is_streaming():
           time.sleep_ms(100)
   finally:
       uvc.reset()
