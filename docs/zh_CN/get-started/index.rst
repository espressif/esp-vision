快速入门
========

:link_to_translation:`en:[English]`

前置条件
--------

- ESP-IDF ``release/v5.5``、``release/v6.0`` 或 ``master``，并已 source 导出脚本，
  使 ``idf.py`` 位于 ``PATH`` 中。
- 一块列在 :doc:`../target-support/index` 中且与所选 target 对应的开发板。

.. only:: esp32s31

   ESP32-S31 当前需要 ESP-IDF ``master`` 环境。

构建、烧录与监视
----------------

带子模块克隆仓库，然后使用 ``make`` 构建：

.. code-block:: bash

   git clone --recursive <this-repo> esp-vision
   cd esp-vision

.. only:: esp32p4

   .. code-block:: bash

      make BOARD=ESP32_P4X_EYE ESPPORT=/dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      make BOARD=ESP32_S3_EYE ESPPORT=/dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      make BOARD=ESP32_S31_KORVO ESPPORT=/dev/ttyACM0 build flash monitor

或在仓库根目录使用板级感知的 ``idf.py`` 扩展：

.. only:: esp32p4

   .. code-block:: bash

      idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      idf.py --board ESP32_S3_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      idf.py --board ESP32_S31_KORVO -p /dev/ttyACM0 build flash monitor

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
