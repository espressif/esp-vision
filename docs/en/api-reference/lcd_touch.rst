lcd_touch -- Touch Input
========================

:link_to_translation:`zh_CN:[中文]`

The ``lcd_touch`` module reads the board's touchscreen independently of display initialization. ``Touch()`` requires configured touch hardware and raises ``OSError`` on unsupported boards.

Read Touch Coordinates
----------------------

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

``read()`` returns a tuple of ``(x, y)`` pixel coordinates, or ``()`` when there is no touch. Coordinates follow the board's touch configuration; image scaling, cropping, and placement must be accounted for by the application.

Ownership and Cleanup
---------------------

The touch device is shared. Creating a new ``Touch()`` transfers ownership to that instance; older instances can no longer read it, and their ``deinit()`` calls do not release the new owner's device. Call ``deinit()`` when finished. Repeated cleanup is safe, and MicroPython soft reset also releases the device.

.. seealso::

   Runnable examples: ``example/06-Peripherals/03-Touch/touch_read.py`` and ``example/06-Peripherals/03-Touch/touch_lcd.py``.

.. include:: _generated/lcd_touch.rst
