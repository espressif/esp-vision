image.ImageIO -- 图像流
=======================

:link_to_translation:`en:[English]`

.. py:currentmodule:: image

``image.ImageIO`` 类型用于读写图像流，可写入文件或内存缓冲。本页内容以
``stubs/imageio.pyi`` 为准。

常量
----

.. py:data:: FILE_STREAM
.. py:data:: MEMORY_STREAM

   :py:meth:`ImageIO.type` 返回的流类型常量。

ImageIO 类
----------

.. py:class:: ImageIO(stream, mode)
              ImageIO(stream, count)

   打开一个图像流。第一种形式打开文件流，``stream`` 为路径，``mode`` 为 ``"r"`` 或
   ``"w"``。第二种形式打开内存流，``stream`` 为 ``(width, height, format)`` 元组，
   ``count`` 为帧数。

   .. py:method:: type()
   .. py:method:: is_closed()
   .. py:method:: count()
   .. py:method:: offset()
   .. py:method:: version()
   .. py:method:: buffer_size()
   .. py:method:: size()

   .. py:method:: write(image)

      向流中追加一帧图像。

   .. py:method:: read(copy_to_fb=True, *, loop=True, pause=True)

      读取下一帧图像；非循环流到末尾时返回 ``None``。

   .. py:method:: seek(offset)
   .. py:method:: sync()
   .. py:method:: close()
