image.ImageIO -- Image Stream
=============================

:link_to_translation:`zh_CN:[中文]`

.. py:currentmodule:: image

The ``image.ImageIO`` type reads and writes image streams, either to a file or to
an in-memory buffer. The source of truth for this page is ``stubs/imageio.pyi``.

Constants
---------

.. py:data:: FILE_STREAM
.. py:data:: MEMORY_STREAM

   Stream type constants returned by :py:meth:`ImageIO.type`.

The ImageIO Class
-----------------

.. py:class:: ImageIO(stream, mode)
              ImageIO(stream, count)

   Open an image stream. The first form opens a file stream where ``stream`` is a
   path and ``mode`` is ``"r"`` or ``"w"``. The second form opens a memory stream
   where ``stream`` is an ``(width, height, format)`` tuple and ``count`` is the
   number of frames.

   .. py:method:: type()
   .. py:method:: is_closed()
   .. py:method:: count()
   .. py:method:: offset()
   .. py:method:: version()
   .. py:method:: buffer_size()
   .. py:method:: size()

   .. py:method:: write(image)

      Append an image to the stream.

   .. py:method:: read(copy_to_fb=True, *, loop=True, pause=True)

      Read the next image, or ``None`` at the end of a non-looping stream.

   .. py:method:: seek(offset)
   .. py:method:: sync()
   .. py:method:: close()
