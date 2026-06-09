display -- LCD Output
=====================

:link_to_translation:`zh_CN:[中文]`

The ``display`` module drives the board LCD. Construct one
:py:class:`ESP32Display` (aliased as ``display.Display``) and push frames to it
with :py:meth:`ESP32Display.write`.

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

.. include:: _generated/display.rst
