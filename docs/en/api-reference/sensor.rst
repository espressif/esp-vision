sensor -- Camera
================

:link_to_translation:`zh_CN:[中文]`

The ``sensor`` module controls the camera and captures frames. It mirrors the
OpenMV ``sensor`` API so existing OpenMV scripts port with little change.

A typical program resets the sensor, selects a pixel format and frame size,
lets the auto-exposure settle, then captures frames in a loop.

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=2000)

   while True:
       img = sensor.snapshot()

.. seealso::

   :doc:`../concepts/camera-pipeline` explains how a frame travels from the
   image sensor to an :py:class:`image.Image`, and :doc:`../concepts/image-model`
   covers pixel formats and color spaces.

.. include:: _generated/sensor.rst
