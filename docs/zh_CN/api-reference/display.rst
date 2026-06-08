display -- LCD 输出
===================

:link_to_translation:`en:[English]`

.. py:module:: display

``display`` 模块驱动板载 LCD。本页内容以 ``stubs/display.pyi`` 为准。

示例
----

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

Display 类
----------

.. py:class:: ESP32Display(width=0, height=0, refresh=60, *, backlight=100)

   创建并初始化板载 LCD。``width`` 与 ``height`` 为 ``0`` 时使用板级默认值；``refresh``
   为目标刷新率（Hz）；``backlight`` 为初始亮度百分比（0-100）。

   ``display.Display`` 是 :py:class:`ESP32Display` 的别名。

   .. py:method:: deinit()

      释放显示驱动资源。

   .. py:method:: width()
   .. py:method:: height()

   .. py:method:: clear(display_off=False)

      清屏。当板卡支持时，``display_off=True`` 可关闭面板输出。

   .. py:method:: backlight(value=None)

      获取或设置背光亮度。``None`` 返回当前值；否则设置 0-100 百分比。

   .. py:method:: write(image, *, x=0, y=0, x_scale=None, y_scale=None, roi=None, fit=True)

      在显示屏上绘制图像。

      :param image: 源图像，通常来自 :py:func:`sensor.snapshot`。
      :param int x: 目标左上角 X（显示像素）。
      :param int y: 目标左上角 Y（显示像素）。
      :param x_scale: 可选的水平缩放系数。
      :param y_scale: 可选的垂直缩放系数。
      :param roi: 可选的源矩形 ``(x, y, w, h)``。
      :param bool fit: ``True`` 将图像缩放以适应显示区域。
