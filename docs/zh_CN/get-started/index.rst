快速入门
========

:link_to_translation:`en:[English]`

前置条件
--------

- ESP-IDF ``release/v5.5``、``release/v6.0`` 或 ``master``，并已 source 导出脚本，
  使 ``idf.py`` 位于 ``PATH`` 中。
- 一块受支持的 ESP32-P4 或 ESP32-S3 目标开发板。

构建、烧录与监视
----------------

带子模块克隆仓库，然后使用 ``make`` 构建：

.. code-block:: bash

   git clone --recursive <this-repo> esp-vision
   cd esp-vision
   make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor

或在仓库根目录使用板级感知的 ``idf.py`` 扩展：

.. code-block:: bash

   idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor

两种入口都会先运行 ``prepare-micropython``：校验 ``lib/micropython`` 已检出到固定的
MicroPython v1.28.0 提交，在 ``build/micropython/`` 下导出干净的 MicroPython 构建副本，
再将 ``overlay/micropython/`` 应用到该副本，``lib/micropython`` 始终保持干净。

常用 Make 目标
--------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - 命令
     - 说明
   * - ``make BOARD=<BOARD> build``
     - 为某块开发板构建固件。
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> deploy``
     - 构建并烧录。
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> monitor``
     - 打开串口监视器。
   * - ``make BOARD=<BOARD> menuconfig``
     - 打开 menuconfig。
   * - ``make BOARD=<BOARD> ESPPORT=<PORT> erase``
     - 擦除 flash。
   * - ``make BOARD=<BOARD> clean``
     - 清理该板的构建输出。
   * - ``make distclean``
     - 清除全部构建输出。

运行第一个脚本
--------------

烧录完成后，通过 REPL 连接并尝试相机功能。可直接运行的脚本请参阅
:doc:`../examples/index`。
