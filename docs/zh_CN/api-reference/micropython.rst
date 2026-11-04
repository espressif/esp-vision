MicroPython API
===============

:link_to_translation:`en:[English]`

ESP-VISION 基于 MicroPython ESP32 port 构建，并直接沿用其语言、标准库、硬件控制、网络、文件系统和运行时 API。这些 API 可以在同一脚本中与 ``sensor``、``image``、``display``、``espdl`` 及其他 ESP-VISION 模块组合使用；ESP-VISION 不会对其进行二次封装或重命名。

文档基线
--------

ESP-VISION 固定使用 MicroPython v1.28.0。应以 `MicroPython v1.28.0 文档 <https://docs.micropython.org/en/v1.28.0/>`_ 为权威 API 参考，重点参阅 `MicroPython 库 <https://docs.micropython.org/en/v1.28.0/library/index.html>`_ 和 `ESP32 快速参考 <https://docs.micropython.org/en/v1.28.0/esp32/quickref.html>`_。MicroPython 面向资源受限设备实现 CPython 功能子集，因此不能将 CPython 文档视为模块内容或行为的精确说明。

语言与内置 API
--------------

ESP-VISION 沿用 MicroPython 语言，以及内置类型、函数、异常、迭代协议、上下文管理器、类和异步语法。详细说明请参阅 `MicroPython 语言与实现 <https://docs.micropython.org/en/v1.28.0/reference/index.html>`_ 和 `内置功能 <https://docs.micropython.org/en/v1.28.0/library/builtins.html>`_。

使用异常和上下文管理器可以提高长时间运行的视觉应用的可恢复性，并确保文件能够确定性关闭：

.. code-block:: python

   try:
       with open("/sdcard/result.txt", "w") as output:
           output.write("capture complete\n")
   except OSError as error:
       print("storage error:", error)

标准库与微型库
--------------

常用的沿用模块包括 ``array``、``asyncio``、``binascii``、``cmath``、``collections``、``errno``、``gc``、``gzip``、``hashlib``、``heapq``、``io``、``json``、``marshal``、``math``、``os``、``platform``、``random``、``re``、``select``、``socket``、``struct``、``sys``、``time``、``weakref``、``zlib`` 和 ``_thread``。ESP-VISION 的开发板 manifest 还会将 MicroPython 的 ``asyncio`` 软件包冻结到固件中。

各模块的类和函数请参阅 `MicroPython 库索引 <https://docs.micropython.org/en/v1.28.0/library/index.html>`_。上游文档中存在的模块仍可能因所选固件配置而被裁减或省略。

JSON 与时间示例
---------------

.. code-block:: python

   import json, time

   result = {
       "timestamp_ms": time.ticks_ms(),
       "objects": 3,
       "confidence": 0.91,
   }
   payload = json.dumps(result)
   print(payload)

``time.ticks_ms()`` 适合测量时间间隔和生成本地运行时间戳；计算两个 tick 值之差时应使用 ``time.ticks_diff()``，以正确处理计数回绕。``json`` 可用于在存储或网络传输前序列化推理结果。

硬件与 ESP32 API
----------------

沿用的 `machine <https://docs.micropython.org/en/v1.28.0/library/machine.html>`_ 模块提供 MCU 控制和外设类，包括 ``Pin``、``Signal``、``ADC``、``ADCBlock``、``PWM``、``I2C``、``SoftI2C``、``SPI``、``SoftSPI``、``UART``、``Timer``、``RTC``、``WDT``、``SDCard``，以及芯片支持时可用的 ``DAC``、``I2S`` 和 ``I2CTarget``。引脚分配和外设冲突取决于开发板，因此将这些 API 与摄像头、显示、存储或编解码硬件组合使用前，应查阅开发板原理图。

沿用的 `esp <https://docs.micropython.org/en/v1.28.0/library/esp.html>`_ 和 `esp32 <https://docs.micropython.org/en/v1.28.0/library/esp32.html>`_ 模块提供 ESP32 port 专用的系统和外设功能。``esp32.PCNT``、``esp32.RMT`` 及其他 SoC 专用模块中各个类的可用性取决于所选芯片和 ESP-IDF 版本。

GPIO 与 PWM 示例
----------------

.. code-block:: python

   from machine import Pin, PWM

   led = Pin(LED_GPIO, Pin.OUT)
   led.value(1)

   pwm = PWM(Pin(PWM_GPIO), freq=1000, duty_u16=32768)
   pwm.duty_u16(49152)
   pwm.deinit()

应将 ``LED_GPIO`` 和 ``PWM_GPIO`` 替换为所选开发板上的空闲引脚。摄像头、显示、存储、USB 和编解码外设可能已经占用部分 GPIO，因此使用前应检查原理图和板级配置。

网络 API
--------

沿用的 `network <https://docs.micropython.org/en/v1.28.0/library/network.html>`_、`socket <https://docs.micropython.org/en/v1.28.0/library/socket.html>`_ 和 `select <https://docs.micropython.org/en/v1.28.0/library/select.html>`_ 模块提供网络接口管理及 TCP/UDP 通信能力。当前产品开发板配置均启用了 ``network.WLAN``，但实际使用仍需要兼容的网络硬件和固件配置。

当前开发板配置关闭了 `bluetooth <https://docs.micropython.org/en/v1.28.0/library/bluetooth.html>`_ 和 `espnow <https://docs.micropython.org/en/v1.28.0/library/espnow.html>`_。SSL/TLS、``cryptolib``、WebSocket 和 socket event 支持取决于构建固件使用的 ESP-IDF 版本。ESP-VISION EV-MUX 运行态不支持传统 MicroPython WebREPL，即使构建包含 ``_webrepl``，其终端与文件操作也不经过 EV-MUX 的 owner 授权。网络控制请使用 :doc:`../device-core/api` 的 TCP 连接；维护中的支持摘要见 :doc:`../target-support/index`。

连接 Wi-Fi
-----------

.. code-block:: python

   import network, time

   wlan = network.WLAN(network.STA_IF)
   wlan.active(True)
   wlan.connect("ssid", "password")

   for _ in range(100):
       if wlan.isconnected():
           break
       time.sleep_ms(100)

   if not wlan.isconnected():
       raise OSError("Wi-Fi connection failed")
   print("address:", wlan.ifconfig()[0])

Wi-Fi 需要兼容的硬件和开发板配置。产品代码应设置超时、避免打印凭据，并处理重新连接，而不是无限期等待。

文件系统与运行时 API
--------------------

ESP-VISION 沿用 MicroPython 的虚拟文件系统和流行为，包括 `os <https://docs.micropython.org/en/v1.28.0/library/os.html>`_、`io <https://docs.micropython.org/en/v1.28.0/library/io.html>`_、`vfs <https://docs.micropython.org/en/v1.28.0/library/vfs.html>`_ 和内置 ``open()`` 函数。当相应分区与硬件可用时，开发板启动代码会挂载 ``/flash``、``/sdcard`` 等产品存储。

运行时服务继续通过 `micropython <https://docs.micropython.org/en/v1.28.0/library/micropython.html>`_、`gc <https://docs.micropython.org/en/v1.28.0/library/gc.html>`_、``sys``、``time`` 和 ``_thread`` 提供。ESP-VISION 启用了带 GIL 的 MicroPython 线程；除非 API 明确说明支持并发使用，视觉对象和硬件资源仍应作为共享资源处理。

列出存储并检查内存
------------------

.. code-block:: python

   import gc, os

   print("flash:", os.listdir("/flash"))
   if "sdcard" in os.listdir("/"):
       print("sdcard:", os.listdir("/sdcard"))

   gc.collect()
   print("free heap:", gc.mem_free())

使用可选挂载点前应先检查其是否存在。加载大型模型或分配图像历史缓冲区前可以调用 ``gc.collect()``，但在每帧循环中反复强制回收可能降低吞吐量。

组织复杂应用
------------

对于熟悉 ESP-IDF 的开发者，``asyncio`` task 是协作调度的 Python 协程，而不是 FreeRTOS task。多个协程共享同一个 Python 执行上下文，并且仅在执行 ``await`` 时切换。控制逻辑、网络 I/O、状态上报和定时工作应优先采用这种结构，因为它可以避免并发访问摄像头、模型、显示和帧缓冲对象。

以下结构由一个协程独占视觉流水线，遥测与健康监控协程只读取复制后的标量结果：

.. blockdiag::

   blockdiag {
     orientation = portrait;

     loop      [label = "MicroPython asyncio 事件循环"];
     vision    [label = "视觉任务\n采集 / 推理 / 预览"];
     state     [label = "复制后的标量状态\n帧数 / 检测数 / 时间戳"];
     telemetry [label = "遥测任务\n序列化并传输"];
     health    [label = "健康监控任务\n检测流水线停滞"];

     loop -> vision;
     loop -> telemetry;
     loop -> health;
     vision -> state;
     state -> telemetry;
     state -> health;
   }

.. code-block:: python

   import asyncio
   import espdl
   import json
   import sensor
   import time

   MODEL = "/sdcard/espdet_pico_224_224_face.espdl"

   state = {
       "frames": 0,
       "detections": 0,
       "last_frame_ms": time.ticks_ms(),
   }
   state_lock = asyncio.Lock()

   async def vision_task(detector):
       while True:
           img = sensor.snapshot()
           results = detector.detect(img)
           for x, y, w, h, score, category in results:
               img.draw_rectangle(x, y, w, h, color=(255, 0, 0))
           img.flush()

           async with state_lock:
               state["frames"] += 1
               state["detections"] = len(results)
               state["last_frame_ms"] = time.ticks_ms()

           # 每帧结束后允许已就绪的控制与网络任务运行。
           await asyncio.sleep_ms(0)

   async def telemetry_task():
       while True:
           await asyncio.sleep_ms(1000)
           async with state_lock:
               payload = json.dumps({
                   "frames": state["frames"],
                   "detections": state["detections"],
               })
           print("telemetry:", payload)
           # 实际应用中应替换为非阻塞 socket 传输。

   async def health_task():
       while True:
           await asyncio.sleep_ms(500)
           async with state_lock:
               age = time.ticks_diff(
                   time.ticks_ms(), state["last_frame_ms"]
               )
           if age > 3000:
               print("warning: vision pipeline stalled")

   async def main():
       sensor.reset()
       sensor.set_pixformat(sensor.RGB565)
       sensor.set_framesize(sensor.QVGA)
       sensor.skip_frames(time=1000)
       detector = espdl.ESPDet(MODEL, score=0.5, nms=0.7)

       vision = asyncio.create_task(vision_task(detector))
       telemetry = asyncio.create_task(telemetry_task())
       health = asyncio.create_task(health_task())
       try:
           await asyncio.gather(vision, telemetry, health)
       finally:
           vision.cancel()
           telemetry.cancel()
           health.cancel()
           detector.deinit()

   asyncio.run(main())

``sensor.snapshot()``、推理、图像处理和 ``img.flush()`` 等调用都是同步操作。执行其中任一调用时，其他协程不能运行；显式调用 ``await asyncio.sleep_ms(0)`` 可在帧与帧之间提供调度点。如果某个网络操作可能阻塞，应使用 ``asyncio`` stream 或非阻塞 socket，而不是长时间执行阻塞调用。

线程与原生任务
--------------

沿用的 ``_thread.start_new_thread()`` API 会创建另一个 FreeRTOS task，但它作为应用接口并不等同于 ``xTaskCreatePinnedToCore()``。Python 在此不提供任务优先级或核亲和性参数；MicroPython 线程固定在 ``MP_TASK_COREID`` 上，并通过 GIL 共享解释器。可以使用 ``_thread.stack_size()`` 配置后续创建线程所分配的栈。

仅当某个阻塞操作无法集成到 ``asyncio`` 时才应使用 ``_thread``，使用 ``_thread.allocate_lock()`` 保护共享 Python 数据；除非 API 明确说明线程安全，否则摄像头、显示、编解码器、模型和帧缓冲应始终由同一个线程持有。IRQ 或其他线程可以通过 ``asyncio.ThreadSafeFlag`` 通知事件循环，而无需直接操作视觉对象。

如果应用需要确定的时延、明确的 FreeRTOS 优先级或核亲和性、ISR 到任务的通知，或者需要在 Python 持有 GIL 时仍持续执行的工作，应将工作任务实现为 ESP-IDF C/C++ 组件，并向 Python 暴露边界清晰的 API。用 ESP-IDF 的思维理解：应用状态机使用 ``asyncio`` task，特殊的阻塞集成使用 ``_thread``，实时服务使用原生 task。

检查固件中的可用性
------------------

标准 MicroPython API 的可用性由 ``overlay/micropython/ports/esp32/mpconfigport.h``、所选开发板的 ``mpconfigboard.h`` 与 ``mpconfigboard.cmake``、ESP-IDF 版本条件和 SoC 能力宏共同决定。由于可用性并非只取决于芯片，最终应以实际固件检查结果为准：

.. code-block:: python

   help("modules")
   import machine
   help(machine)

应用需要跨多块 ESP-VISION 开发板运行时，应使用 ``hasattr()`` 或受保护的 import。产品固件需要移除或增加沿用 API 时，请按照 :doc:`../how-to/customize-firmware` 操作。

.. seealso::

   使用标准 MicroPython API 的可运行示例包括 ``example/00-HelloWorld/helloworld.py``\ （首个脚本）和 ``example/06-Peripherals/00-Storage/sdcard.py``\ （需要 SD 卡）。``example/07-Network/00-WebREPL/webrepl.py`` 用于说明传统 WebREPL 不受支持的边界，会在改变网络前退出，不是 ESP-VISION 上可运行的网络终端。
