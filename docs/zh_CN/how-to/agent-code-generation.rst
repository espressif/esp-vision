Agent 代码生成指南
========================

:link_to_translation:`en:[English]`

本指南是 AI Agent 生成 ESP-VISION 应用代码时的强制性约束。必须将目标环境视为在 ESP32 微控制器上运行的 ESP-VISION MicroPython v1.28.0，而不是运行 CPython 的 PC。只能使用本知识包中存在、已为所选开发板启用，或已由仓库示例验证的 API；不得假定设备上存在桌面端标准库模块、``pip`` 包、原生 wheel、操作系统服务、子进程或 PC 文件系统。

依赖规则
--------

默认 ESP-VISION 固件不能从 ``requirements.txt`` 动态安装依赖，也没有包含设备端 ``mip``。生成的目标端代码不得调用 ``pip``、解析 ``requirements.txt``、调用 ``mip.install()``，也不得静默依赖未验证的第三方包。如果知识包中没有必需模块，应改用已支持的模块重写，或明确说明运行前必须通过主机工具复制到 ``/lib``、预编译为兼容的 ``.mpy``、冻结到定制固件，或实现为原生模块。

主机端工具与目标端代码属于两个不同环境。PC 工具可以使用 ``pyserial`` 等 CPython 包，但必须将这类代码标记为“仅主机端”，且不能在 ESP-VISION 脚本中导入这些包。

All-in-One 输出
------------------

除非用户明确要求软件包或多文件架构，否则应返回一个可独立运行的设备端 ``main.py``。将已支持的 import、常量、初始化、应用逻辑、主循环或有界测试、异常处理和资源释放都放在该文件中，避免要求用户再手工创建辅助模块。模型和其他二进制资产可以保持为独立文件，但必须列出它们的精确设备路径和部署前置条件。

在进入无限产品循环前，优先生成有界且可观测的验证代码。只有在摄像头、模型、存储、网络或用户要求的其他操作真正成功后，才打印唯一成功标记。下面的目标端冒烟测试是 all-in-one MicroPython 脚本，只有在成功拍摄一帧后才打印 ``EVTEST:OK``：

.. code-block:: python

   import gc
   import sensor


   def main():
       print("EVTEST:START")
       sensor.reset()
       try:
           sensor.set_pixformat(sensor.RGB565)
           sensor.set_framesize(sensor.QQVGA)
           sensor.skip_frames(time=500)
           image = sensor.snapshot()
           width = image.width()
           height = image.height()
       finally:
           sensor.shutdown()
       print("EVTEST:OK {}x{} heap={}".format(
           width, height, gc.mem_free()))


   main()

设备通信与闭环验收
--------------------

Agent 与 IDE、CLI 同属主机应用，设备交互使用 Device Core，不再维护另一套 EV-MUX 客户端；统一的主机/Core/协议/固件关系见 :doc:`../architecture/index`。生成代码后的设备接管、上传、执行与结果核验统一遵循 :doc:`../communication/index`。完整主机端摄像头闭环示例已移至 :doc:`../communication/validation`；不得用握手、上传 ACK、命令回显或 REPL 提示符替代应用成功证据。
