display -- LCD 显示
===================

:link_to_translation:`en:[English]`

``display`` 模块驱动开发板上的 LCD。创建一个 :py:class:`ESP32Display`\ （别名为 ``display.Display``\ ），并用 :py:meth:`ESP32Display.write` 把图像帧送到屏幕。

摄像头预览
----------

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

使用 ``fit=True`` 时，源图像会缩放以适配屏幕，并沿用显示驱动的默认放置方式。显示对象应只创建一次并持续复用；重复创建对象会重新初始化屏幕资源。

复用 Image 绘制内容
-------------------

如果应用需要自行绘制图形，可以创建一个普通的 RGB565 图像缓冲区并重复更新，再传给 ``lcd.write()``：

.. code-block:: python

   import display, image

   lcd = display.Display()
   canvas = image.Image(lcd.width(), lcd.height(), image.RGB565)
   canvas.draw_rectangle(0, 0, canvas.width(), canvas.height(), color=(6, 8, 18), fill=True)
   canvas.draw_string(20, 20, "ESP-VISION", color=(255, 255, 255), scale=2)
   lcd.write(canvas, fit=False)

后续帧可以继续修改并复用同一个 ``Image``。显示驱动会将图像像素写入屏幕；图像无需分配在显示驱动的帧缓冲区中。

定位、缩放与裁剪
----------------

使用 ``x`` 和 ``y`` 设置显示位置，使用 ``x_scale`` 和 ``y_scale`` 显式缩放，并通过 ``roi`` 仅显示源图像中的指定区域：

.. code-block:: python

   img = sensor.snapshot()
   lcd.write(
       img,
       x=20,
       y=10,
       x_scale=0.5,
       y_scale=0.5,
       roi=(40, 30, 160, 120),
       fit=False,
   )

当 ``fit=False`` 时，输出由显式指定的位置和缩放比例控制。ROI 使用源图像坐标，格式为 ``(x, y, width, height)``；缩小 ROI 还可以减少送入显示链路的图像数据量。

背光与资源释放
--------------

.. code-block:: python

   lcd = display.Display(backlight=80)
   print("panel:", lcd.width(), "x", lcd.height())
   lcd.backlight(30)
   lcd.clear()
   lcd.deinit()

背光值为 0 到 100 的百分比。应用永久释放显示设备时应调用 ``deinit()``；驱动反初始化后不得继续写入图像帧。

.. seealso::

   可运行示例：``example/06-Peripherals/01-Display/lcd_preview.py``\ 。

.. include:: _generated/display.rst
