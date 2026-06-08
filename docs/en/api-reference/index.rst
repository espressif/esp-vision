API Reference
=============

:link_to_translation:`zh_CN:[中文]`

ESP-VISION exposes its functionality through C-implemented MicroPython modules.
The public module names stay OpenMV-compatible: the camera module is ``sensor``,
with ``image``, ``display``, and ``espdl`` alongside it. The codec stream type is
exposed as ``image.ImageIO``.

The pages below are the reference for each module. They are kept in sync with the
type stubs under ``stubs/``, which are also usable for IDE completion.

.. toctree::
   :maxdepth: 1

   sensor
   image
   display
   espdl
   imageio
   h264
   rtsp
