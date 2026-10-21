display -- LCD Output
=====================

:link_to_translation:`zh_CN:[中文]`

The ``display`` module drives the board LCD. Construct one :py:class:`ESP32Display` (aliased as ``display.Display``) and push frames to it with :py:meth:`ESP32Display.write`.

Camera Preview
--------------

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

With ``fit=True``, the source image is scaled to fit the panel while preserving the display driver's default placement behavior. Create the display once and reuse it; repeatedly constructing the object reinitializes panel resources.

Drawing into a Reusable Image
-----------------------------

For application-generated graphics, allocate a regular RGB565 image buffer once, update it, and pass it to ``lcd.write()``:

.. code-block:: python

   import display, image

   lcd = display.Display()
   canvas = image.Image(lcd.width(), lcd.height(), image.RGB565)
   canvas.draw_rectangle(0, 0, canvas.width(), canvas.height(), color=(6, 8, 18), fill=True)
   canvas.draw_string(20, 20, "ESP-VISION", color=(255, 255, 255), scale=2)
   lcd.write(canvas, fit=False)

Keep and redraw the same ``Image`` when producing later frames. The display driver copies its pixels to the panel; the image does not need to be allocated in the driver's framebuffer.

Position, Scale, and Crop
-------------------------

Use ``x`` and ``y`` for placement, ``x_scale`` and ``y_scale`` for explicit scaling, and ``roi`` to display only a source region:

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

When ``fit=False``, the explicit position and scale values control the output. The ROI is expressed in source-image coordinates as ``(x, y, width, height)``; reducing the ROI also reduces the amount of image data sent to the display pipeline.

Backlight and Cleanup
---------------------

.. code-block:: python

   lcd = display.Display(backlight=80)
   print("panel:", lcd.width(), "x", lcd.height())
   lcd.backlight(30)
   lcd.clear()
   lcd.deinit()

Backlight values are percentages from 0 to 100. Call ``deinit()`` when an application permanently releases the display; do not write frames after the driver has been deinitialized.

.. seealso::

   Runnable example: ``example/06-Peripherals/01-Display/lcd_preview.py``.

.. include:: _generated/display.rst
