快速入门
========

:link_to_translation:`en:[English]`

前置条件
--------

- 受支持的 ESP-IDF 环境，并已 source 导出脚本，使 ``idf.py`` 位于 ``PATH`` 中；当前分支兼容性请查看项目 README。
- 一块列在 :doc:`../target-support/index` 中且与所选芯片对应的开发板。

构建、烧录与监视
----------------

带子模块克隆仓库，然后在仓库根目录使用板级感知的 ``idf.py`` 扩展：

.. code-block:: bash

   git clone --recursive https://github.com/espressif/esp-vision.git esp-vision
   cd esp-vision

.. only:: esp32p4

   .. code-block:: bash

      idf.py --board ESP32_P4X_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s3

   .. code-block:: bash

      idf.py --board ESP32_S3_EYE -p /dev/ttyACM0 build flash monitor

.. only:: esp32s31

   .. code-block:: bash

      idf.py --board ESP32_S31_KORVO -p /dev/ttyACM0 build flash monitor

该命令会先运行 ``prepare-micropython``：校验 ``lib/micropython`` 已检出到固定的 MicroPython v1.28.0 提交，在 ``build/micropython/`` 下导出干净的 MicroPython 构建副本，再将 ``overlay/micropython/`` 应用到该副本，然后将每个 ``boards/<BOARD>/port/`` 投射到该副本的 ``ports/esp32/boards/<BOARD>/``。``lib/micropython`` 始终保持干净。

常用 idf.py 命令
----------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - 命令
     - 说明
   * - ``idf.py --board <BOARD> build``
     - 为某块开发板构建固件。
   * - ``idf.py --board <BOARD> -p <PORT> flash``
     - 构建并烧录固件。
   * - ``idf.py --board <BOARD> -p <PORT> monitor``
     - 打开串口监视器。
   * - ``idf.py --board <BOARD> menuconfig``
     - 打开 menuconfig。
   * - ``idf.py --board <BOARD> -p <PORT> erase-flash``
     - 擦除 flash。
   * - ``idf.py --board <BOARD> clean``
     - 清理该板的构建输出。
   * - ``idf.py --board <BOARD> fullclean``
     - 删除所选开发板的完整构建目录。

运行第一个脚本
--------------

烧录完成后，通过 REPL 连接并尝试相机功能。每个 :doc:`../api-reference/index` 模块页面都会链接到对应 API 的可运行 ``example/`` 脚本。
