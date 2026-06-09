rtsp -- RTSP Streaming
======================

:link_to_translation:`zh_CN:[中文]`

The ``rtsp`` module serves H.264 NAL units over RTSP so a client (for example
VLC or ffplay) can view the camera stream over the network. Pair it with
:doc:`h264`.

.. code-block:: python

   import sensor, h264, rtsp

   enc = h264.H264Encoder(320, 240, fps=15)
   server = rtsp.RTSPServer(320, 240, fps=15)
   while True:
       server.send(enc.encode(sensor.snapshot()))

.. seealso::

   :doc:`../concepts/codec-streaming` covers the full capture, encode, and
   stream pipeline.

.. include:: _generated/rtsp.rst
