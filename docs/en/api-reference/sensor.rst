sensor -- Camera
================

:link_to_translation:`zh_CN:[中文]`

.. py:module:: sensor

The ``sensor`` module controls the camera and captures frames. It mirrors the
OpenMV ``sensor`` API. The source of truth for this page is ``stubs/sensor.pyi``.

Example
-------

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=2000)

   img = sensor.snapshot()

Constants
---------

.. py:data:: GRAYSCALE
.. py:data:: RGB565

   Pixel format constants accepted by :py:func:`set_pixformat`.

.. py:data:: QQVGA
.. py:data:: QVGA

   Frame size constants accepted by :py:func:`set_framesize`.

Functions
---------

.. py:function:: reset()

   Reset and initialize the camera with the board default sensor configuration.

.. py:function:: shutdown(enable=True)

   Stop or restart the camera stream.

   :param bool enable: ``True`` shuts the camera down; ``False`` starts it again.

.. py:function:: set_pixformat(pixformat)

   Select the output pixel format.

   :param int pixformat: :py:data:`GRAYSCALE` or :py:data:`RGB565`.

.. py:function:: get_pixformat()

   :return: the current pixel format constant.
   :rtype: int

.. py:function:: set_framesize(framesize)

   Select the output frame size.

   :param int framesize: :py:data:`QQVGA` or :py:data:`QVGA`.

.. py:function:: get_framesize()

   :return: the current frame size constant.
   :rtype: int

.. py:function:: width()

   :return: the current output image width in pixels.
   :rtype: int

.. py:function:: height()

   :return: the current output image height in pixels.
   :rtype: int

.. py:function:: get_id()

   :return: the camera sensor ID.
   :rtype: int

.. py:function:: set_hmirror(enable)

   Mirror the camera image horizontally.

   :param bool enable: ``True`` enables horizontal mirror.

.. py:function:: get_hmirror()

   :return: whether horizontal mirror is enabled.
   :rtype: bool

.. py:function:: set_vflip(enable)

   Flip the camera image vertically.

   :param bool enable: ``True`` enables vertical flip.

.. py:function:: get_vflip()

   :return: whether vertical flip is enabled.
   :rtype: bool

.. py:function:: skip_frames(time=0, n=0)

   Drop frames while camera exposure and processing settle.

   :param int time: milliseconds to wait.
   :param int n: number of frames to skip.

.. py:function:: snapshot(buffer=None)

   Capture one image from the camera.

   :param buffer: reserved for compatibility; pass ``None`` in current builds.
   :return: the captured image.
   :rtype: image.Image

.. py:function:: status()

   :return: camera readiness, size, format, mirror, flip, and crop status as a
            dictionary with keys ``ready``, ``id``, ``width``, ``height``,
            ``pixformat``, ``hmirror``, ``vflip``, ``raw_width``, ``raw_height``,
            ``active_width``, and ``active_height``.
   :rtype: dict
