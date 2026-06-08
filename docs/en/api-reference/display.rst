display -- LCD Output
=====================

:link_to_translation:`zh_CN:[中文]`

.. py:module:: display

The ``display`` module drives the board LCD. The source of truth for this page is
``stubs/display.pyi``.

Example
-------

.. code-block:: python

   import sensor, display

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)

   lcd = display.ESP32Display()
   while True:
       lcd.write(sensor.snapshot(), fit=True)

The Display Class
-----------------

.. py:class:: ESP32Display(width=0, height=0, refresh=60, *, backlight=100)

   Create and initialize the board LCD. ``width`` and ``height`` of ``0`` use the
   board default; ``refresh`` is the target refresh rate in Hz; ``backlight`` is
   the initial brightness percentage (0-100).

   ``display.Display`` is an alias for :py:class:`ESP32Display`.

   .. py:method:: deinit()

      Release the display driver resources.

   .. py:method:: width()
   .. py:method:: height()

   .. py:method:: clear(display_off=False)

      Clear the display. ``display_off=True`` may turn the panel output off when
      the board supports it.

   .. py:method:: backlight(value=None)

      Get or set backlight brightness. ``None`` returns the current value;
      otherwise set 0-100 percent.

   .. py:method:: write(image, *, x=0, y=0, x_scale=None, y_scale=None, roi=None, fit=True)

      Draw an image on the display.

      :param image: source image, normally from :py:func:`sensor.snapshot`.
      :param int x: destination top-left X in display pixels.
      :param int y: destination top-left Y in display pixels.
      :param x_scale: optional manual horizontal scale factor.
      :param y_scale: optional manual vertical scale factor.
      :param roi: optional source rectangle ``(x, y, w, h)``.
      :param bool fit: ``True`` scales the image to fit the display area.
