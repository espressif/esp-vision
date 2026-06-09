display -- LCD 显示
===================

:link_to_translation:`en:[English]`

``display`` 模块驱动开发板上的 LCD。创建一个 :py:class:`ESP32Display`\ （别名为
``display.Display``\ ），并用 :py:meth:`ESP32Display.write` 把图像帧送到屏幕。

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

.. include:: _generated/display.rst
