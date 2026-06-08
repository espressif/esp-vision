添加新的开发板
==============

:link_to_translation:`en:[English]`

一块开发板的定义分布在两处：MicroPython ESP32 移植 overlay
（``overlay/micropython/ports/esp32/boards/<BOARD>/``）与 ESP-VISION 仓库
（``boards/<BOARD>/``）。建议从 ``TEMPLATE`` 板开始。

MicroPython 移植侧
------------------

位于 ``overlay/micropython/ports/esp32/boards/<BOARD>/``：

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - 文件
     - 用途
   * - ``mpconfigboard.cmake``
     - IDF 目标与 ``SDKCONFIG_DEFAULTS`` 链。
   * - ``mpconfigboard.h``
     - MicroPython 功能开关与 USB 字符串。
   * - ``sdkconfig.board``
     - 板级 ESP-IDF Kconfig 覆盖项。
   * - ``partitions-*.csv``
     - 分区表。
   * - ``board.json``、``board.md``
     - 上游板卡清单元数据。

ESP-VISION 侧
-------------

位于 ``boards/<BOARD>/``：

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - 文件
     - 用途
   * - ``boardconfig.h``
     - 引脚分配与板级运行时常量。
   * - ``imlib_config.h``
     - OpenMV ``imlib`` 功能开关。
   * - ``manifest.py``
     - 冻结的 Python 模块。
   * - ``camera.c``
     - 板级相机后端。
   * - ``display.c``
     - LCD 面板与背光实现。
   * - ``sdcard.c``
     - SD 卡供电与插卡检测实现。

当板目录下存在 ``camera.c``、``display.c``、``sdcard.c`` 时，``micropython.cmake`` 会
自动选用它们，并包含板卡可选的 ``board.cmake``。

构建与烧录
----------

.. code-block:: bash

   make BOARD=<NEW_BOARD> ESPPORT=/dev/ttyACM0 build flash monitor
   idf.py --board <NEW_BOARD> -p /dev/ttyACM0 build flash monitor

.. note::

   本页为起步提纲。详细的适配步骤（传感器选择、PPA 配置、显示时序）将在后续补全。
