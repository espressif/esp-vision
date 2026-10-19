快速入门
========

:link_to_translation:`en:[English]`

前置条件
--------

- ESP32-P4/S3 开发板使用 ESP-IDF **v5.5.6**，ESP32-S31 开发板使用 **release/v6.1**；请 source 对应 checkout 的 ``export.sh``，使 ``idf.py`` 位于 ``PATH`` 中。
- 一块列在 :doc:`../target-support/index` 中且与所选芯片对应的开发板。

.. warning::

   CI 和发布固件仅验证上述开发板与 IDF 组合。v5.5.6 固定到补丁版本；S31 跟随 ``release/v6.1`` 维护分支。其它 IDF 版本可能涉及 ESP-IDF 或托管组件 API 变更，导致编译失败、运行行为变化，或改变可用的 MicroPython API；可能需要适配源码、overlay 或依赖，并重新进行硬件验证。可选 MicroPython 功能差异见项目 README。

.. note::

   在 Windows 平台编译时，请先安装 `Visual Studio Build Tools <https://visualstudio.microsoft.com/visual-cpp-build-tools/>`_，再运行 ``idf.py``。在 Visual Studio Build Tools 安装器中，请勾选 “Desktop development with C++”（使用 C++ 的桌面开发）工作负载，并确认已勾选 “MSVC v143 - VS 2022 C++ x64/x86 build tools” 组件。

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
