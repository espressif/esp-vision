API 参考
========

:link_to_translation:`en:[English]`

ESP-VISION 通过以 C 实现的 MicroPython 模块对外暴露功能。公开模块名保持与 OpenMV 兼容：
相机模块为 ``sensor``，并提供 ``image``、``display`` 与 ``espdl``。编解码流类型以
``image.ImageIO`` 暴露。

下列页面为各模块的参考文档，与 ``stubs/`` 下的类型存根保持同步，存根同样可用于 IDE 补全。

.. toctree::
   :maxdepth: 1

   sensor
   image
   display
   espdl
   imageio
   h264
   rtsp
