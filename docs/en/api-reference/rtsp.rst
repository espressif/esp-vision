rtsp -- RTSP Streaming
======================

:link_to_translation:`zh_CN:[中文]`

.. py:module:: rtsp

The ``rtsp`` module serves H.264 NAL units over RTSP so a client can view the
camera stream over the network. Pair it with :doc:`h264`. The source of truth for
this page is ``stubs/rtsp.pyi``.

Example
-------

.. code-block:: python

   import sensor, h264, rtsp

   enc = h264.H264Encoder(320, 240, fps=15)
   server = rtsp.RTSPServer(320, 240, fps=15)
   while True:
       server.send(enc.encode(sensor.snapshot()))

The RTSPServer Class
--------------------

.. py:class:: RTSPServer(width, height, *, fps=15, listen_port=8554, max_frame_len=0)

   Start an RTSP server for ``width`` x ``height`` frames on ``listen_port``.

   .. py:method:: send(nal)

      Send one H.264 NAL unit (bytes) to connected clients.

   .. py:method:: stop()

      Stop the server.
