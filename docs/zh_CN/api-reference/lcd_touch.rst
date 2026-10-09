lcd_touch -- 触摸输入
=====================

:link_to_translation:`en:[English]`

``lcd_touch`` 模块读取开发板的触摸屏，无需先初始化显示设备。``Touch()`` 要求开发板已配置触摸硬件，不支持的开发板会抛出 ``OSError``。

读取触摸坐标
------------

.. code-block:: python

   import lcd_touch
   import time

   touch = lcd_touch.Touch()
   try:
       while True:
           points = touch.read()
           if points:
               print(points)
           time.sleep_ms(20)
   finally:
       touch.deinit()

``read()`` 返回由 ``(x, y)`` 像素坐标组成的元组，没有触点时返回 ``()``。坐标遵循开发板的触摸配置；应用需要自行处理图像缩放、裁剪和显示位置对应的坐标转换。

所有权与资源释放
----------------

触摸设备由实例共享。创建新的 ``Touch()`` 会将所有权转移给新实例；旧实例无法继续读取，其 ``deinit()`` 调用也不会释放新实例持有的设备。使用完毕后应调用 ``deinit()``，重复调用是安全的，MicroPython 软复位也会释放设备。

.. seealso::

   可运行示例：``example/06-Peripherals/03-Touch/touch_read.py`` 和 ``example/06-Peripherals/03-Touch/touch_lcd.py``。

.. include:: _generated/lcd_touch.rst
