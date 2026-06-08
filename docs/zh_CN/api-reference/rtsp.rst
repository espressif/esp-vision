rtsp -- RTSP 推流
=================

:link_to_translation:`en:[English]`

.. py:module:: rtsp

``rtsp`` 模块通过 RTSP 提供 H.264 NAL 单元，使客户端可在网络上观看相机画面。通常与
:doc:`h264` 配合使用。本页内容以 ``stubs/rtsp.pyi`` 为准。

示例
----

.. code-block:: python

   import sensor, h264, rtsp

   enc = h264.H264Encoder(320, 240, fps=15)
   server = rtsp.RTSPServer(320, 240, fps=15)
   while True:
       server.send(enc.encode(sensor.snapshot()))

RTSPServer 类
-------------

.. py:class:: RTSPServer(width, height, *, fps=15, listen_port=8554, max_frame_len=0)

   为 ``width`` x ``height`` 的帧在 ``listen_port`` 上启动 RTSP 服务。

   .. py:method:: send(nal)

      向已连接的客户端发送一个 H.264 NAL 单元（bytes）。

   .. py:method:: stop()

      停止服务。
