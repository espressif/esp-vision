rtsp -- RTSP 推流
=================

:link_to_translation:`en:[English]`

``rtsp`` 模块通过 RTSP 协议发送 H.264 NAL 单元，使客户端（如 VLC 或 ffplay）能在
网络上观看摄像头画面。需与 :doc:`h264` 配合使用。

.. code-block:: python

   import sensor, h264, rtsp

   enc = h264.H264Encoder(320, 240, fps=15)
   server = rtsp.RTSPServer(320, 240, fps=15)
   while True:
       server.send(enc.encode(sensor.snapshot()))

.. seealso::

   :doc:`../concepts/codec-streaming` 完整介绍了采集、编码与推流的整个流程。

.. include:: _generated/rtsp.rst
