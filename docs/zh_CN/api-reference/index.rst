API 参考
========

:link_to_translation:`en:[English]`

ESP-VISION 通过以 C 实现的 MicroPython 模块对外暴露功能。公开模块名保持与 OpenMV 兼容：
相机模块为 ``sensor``，并提供 ``image``、``display`` 与 ``espdl``。编解码流类型以
``image.ImageIO`` 暴露。本页只列出所选 target 默认编译的模块。

下列页面为各模块的参考文档，与 ``stubs/`` 下的类型存根保持同步，存根同样可用于 IDE
补全。模块可用性在文档构建时根据 ``micropython.cmake`` 自动推导。

本指南覆盖 ESP-VISION 模块。标准 MicroPython 模块由
``overlay/micropython/ports/esp32/mpconfigport.h``、板级
``mpconfigboard.h`` 覆盖项、ESP-IDF 版本条件和 SoC 能力宏共同决定，因此仅凭 target
不一定能准确判断其可用性。

.. include:: _generated/module-toctrees.rst
