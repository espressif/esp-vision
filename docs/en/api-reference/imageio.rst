image.ImageIO -- Image Stream
=============================

:link_to_translation:`zh_CN:[中文]`

The ``image.ImageIO`` type records and replays sequences of images while
preserving inter-frame timing. A file stream reads/writes a container on
storage; a memory stream keeps frames in a pre-allocated buffer. It is exposed
on the ``image`` module as ``image.ImageIO``; the type stub lives in
``stubs/imageio.pyi``.

.. code-block:: python

   import sensor, image

   stream = image.ImageIO("/sdcard/stream.bin", "w")
   for _ in range(30):
       stream.write(sensor.snapshot())
   stream.close()

.. seealso::

   :doc:`../concepts/codec-streaming` covers ImageIO alongside JPEG, H.264, and
   the USB CDC preview path.

.. include:: _generated/imageio.rst
