启动流程
========

:link_to_translation:`en:[English]`

ESP-VISION 沿用 MicroPython 的复位与启动模型，并增加 Flash 文件系统、板级存储、摄像头、显示和预览服务的初始化。本章说明当前固件实际实现的启动顺序。关于上游模型和通用行为，请参阅 `MicroPython v1.28.0 复位与启动流程 <https://docs.micropython.org/en/v1.28.0/reference/reset_boot.html>`_。

硬复位与软复位
--------------

硬复位会先重启 MCU 和 ESP-IDF 运行时，再创建新的 MicroPython 环境。上电、开发板复位按键、``machine.reset()``、看门狗或欠压复位以及从深度睡眠唤醒都会触发硬复位。应用需要区分复位来源时，可以使用 ``machine.reset_cause()``。

软复位在不重启完整 MCU 运行时的情况下重新启动 MicroPython 环境，可通过友好 REPL 中的 ``Ctrl-D`` 或 ``machine.soft_reset()`` 触发。ESP-VISION 会清除 Python 对象和模块，通过 MicroPython 清理过程关闭文件和 socket，释放摄像头、显示、预览、USB、PWM、定时器、UART、线程及其他托管资源，然后重新执行 Python 启动流程。

部分系统状态可能在软复位后保留，包括 RTC、CPU 时钟配置，以及 IP 层仍处于活动状态的网络接口。应用代码不能假定表示这些资源的 Python 对象仍然存在；每次复位后都应重新创建对象并检查其状态。

ESP-VISION 启动顺序
-------------------

硬复位或软复位后，ESP-VISION 执行以下启动流程：

.. blockdiag::

   blockdiag {
     orientation = portrait;

     reset    [label = "硬复位或软复位"];
     storage  [label = "通过 ESP-IDF storage 挂载\nFlash 和可用的 SD 卡"];
     runtime  [label = "初始化 MicroPython 运行时\n和 machine 外设"];
     bridge   [label = "在 / 和 /sdcard 发布\n原生 VFS bridge"];
     services [label = "初始化摄像头、显示、\n预览和帧缓冲服务"];
     boot     [label = "执行冻结的 _boot.py\n运行清理工作"];
     setup    [label = "执行冻结的 py_inisetup.py\n创建缺失的默认文件"];
     bootpy   [label = "存在时执行 /boot.py"];
     usb      [label = "初始化 USB 设备"];
     replmode [label = "REPL 模式", shape = diamond];
     mainpy   [label = "存在时执行 /main.py"];
     repl     [label = "进入友好或原始 REPL"];

     reset -> storage -> runtime -> bridge -> services -> boot -> setup -> bootpy -> usb -> replmode;
     replmode -> mainpy [label = "友好模式"];
     replmode -> repl [label = "原始模式"];
     mainpy -> repl [label = "退出、中断或跳过"];
   }

主机自动化工具使用的原始 REPL 可以在软复位时跳过 ``main.py``，从而使开发工具无需自动启动产品应用即可取得设备控制权。

首次启动与 Flash 文件系统
-------------------------

ESP-IDF storage manager 独立于 MicroPython VM 持有开发板文件系统。它在内部将 ``ffat`` 分区挂载到 ``/flash``，并将可用的 SD 卡挂载到 ``/sdcard``。每次 VM 启动时，原生 MicroPython VFS bridge 会把 Flash 卷发布到 ``/``，把 SD 卷发布到 ``/sdcard``。软复位只会重新创建 bridge 对象，不会重新挂载已挂载的物理文件系统；如果 SD 卡在冷启动时未挂载，后续每次 VM 启动都会重试，因此开机后插入的 SD 卡可在软复位后变为可用。

storage manager 会在初始化时将未格式化的 ``ffat`` 分区格式化为 FAT。兼容旧固件的 ``vfs`` 分区只有在已经包含 FAT 时才会挂载，绝不会自动格式化，因此不会静默擦除可能存在的 LittleFS2 卷。其他挂载或写入失败只会上报，不会执行破坏性修复。随后，冻结的 ``py_inisetup.py`` 会在对应文件不存在时创建 ``/boot.py``、``/main.py``、``/README.txt`` 和 ``/.esp_vision_disk`` 标记文件。

正常固件升级或软复位不会覆盖已有启动文件。开发板包可以通过 ``boards/<BOARD>/board_inisetup.py`` 提供板级默认 ``main.py`` 和 ``README.txt`` 内容。

使用 boot.py
------------

``boot.py`` 适合执行必须在应用启动前完成、耗时短且结果确定的初始化，例如选择产品模式、准备配置或启动必需的网络接口：

.. code-block:: python

   import network

   wlan = network.WLAN(network.STA_IF)
   wlan.active(True)

``boot.py`` 必须返回，不应包含应用的永久循环。ESP-VISION 仅在 ``boot.py`` 完成后初始化 MicroPython USB 设备，因此阻塞或长时间运行的 ``boot.py`` 可能导致 USB REPL 和主机工具无法使用。

使用 main.py
------------

``main.py`` 应作为产品应用入口。建议将具体实现放在独立模块中，使启动策略与应用逻辑彼此独立：

.. code-block:: python

   import sys
   import my_app

   try:
       my_app.main()
   except KeyboardInterrupt:
       raise
   except Exception as error:
       print("Fatal application error:")
       sys.print_exception(error)

允许 ``KeyboardInterrupt`` 继续抛出，可以使用 ``Ctrl-C`` 停止应用并进入友好 REPL。产品应用需要自动恢复时，也可以记录异常后调用 ``machine.reset()``，但无条件复位循环会增加开发和故障诊断难度。

ESP-VISION 默认 ``main.py`` 会输出开发板就绪信息并在循环中休眠，使 VSCode 扩展和其他主机工具能够取得控制权。可按 ``Ctrl-C`` 中断该脚本并进入 REPL，或使用产品入口替换 ``/main.py``。

REPL 与故障恢复
---------------

``main.py`` 正常退出或抛出未捕获异常后，控制权会转交给友好 REPL。``Ctrl-C`` 会向正在运行的 Python 脚本注入 ``KeyboardInterrupt``，在友好 REPL 中按 ``Ctrl-D`` 则会启动软复位。

如果应用导致设备无法正常启动，可以连接 REPL，使用 ``Ctrl-C`` 中断应用，然后重命名或删除启动文件：

.. code-block:: python

   import os

   print(os.listdir("/"))
   os.rename("/main.py", "/main.disabled.py")
   # 使用 Ctrl-D 重新启动，并避免运行之前的 main.py。

如果无法恢复 REPL，应在仓库根目录擦除并重新烧录开发板。``erase-flash`` 会清除设备的全部 Flash，包括内部文件系统和用户数据：

.. code-block:: bash

   idf.py --board <BOARD> -p <PORT> erase-flash
   idf.py --board <BOARD> -p <PORT> flash monitor

不执行 ``erase-flash`` 的重新烧录通常会保留数据文件系统，因此也可能保留存在问题的 ``boot.py`` 或 ``main.py``。
