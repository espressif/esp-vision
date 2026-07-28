Agent 代码生成与 EV-MUX 闭环测试
===================================

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

USB-OTG CDC 与 USJ 传输约定
---------------------------

ESP-VISION 在 USB-OTG CDC（sink ``cdc``）和 USB-Serial-JTAG（sink ``usj``）上承载相同的 EV-MUX 协议。每次测试只打开一个设备端口。以 DTR 有效状态打开 CDC 端口后，CDC 会成为自动路由；否则支持的开发板会回退到 USJ。测试时不要同时打开两个端口，也不要保留其他串口监视器连接。USB 波特率不属于协议，但主机串口 API 仍需一个占位波特率。

每个帧由 ``0x1e``、``EVMUX/1 h=<metadata_len> p=<payload_len> c=<crc32>\r\n``、精确 ``h`` 字节的 UTF-8 JSON metadata、精确 ``p`` 字节的 payload 以及最后的 ``0x1f`` 组成。metadata 和 payload 边界由长度而不是分隔符决定。设备帧包含 payload CRC32；主机命令可发送零 CRC，但建议仍计算 CRC。接收端必须在 ``0x1e`` 上重新同步，验证长度与 CRC，然后按 ``metadata.channel`` 分发每个帧，不能按 USB 端口或到达顺序推断帧类型。

最小闭环是：打开所选端口；交换 ``hello`` 与 ``capabilities``；验证 ``firmware.id == "esp-vision"`` 及协商的 ``evMuxVersion``；查询 ``transport.state``；停止当前脚本以进入 REPL；通过 ``script.write`` 上传一个完整的设备脚本；通过 ``script.run`` 启动；并必须在 ``repl.stdout`` 上看到期望标记。``script.run`` 响应只表示脚本已入队，不能单独作为应用测试成功的依据。

.. seqdiag::

   seqdiag {
     "主机"; "设备传输层"; "MicroPython VM";

     "主机" -> "设备传输层" [label = "hello / capabilities"];
     "设备传输层" --> "主机" [label = "身份 / 协议能力"];
     "主机" -> "设备传输层" [label = "transport.state / stop"];
     "主机" -> "MicroPython VM" [label = "script.write 分块"];
     "主机" -> "MicroPython VM" [label = "script.run"];
     "MicroPython VM" --> "主机" [label = "repl.stdout: EVTEST:OK"];
   }

下面的仅主机端 CPython 脚本对 USB-OTG CDC 或 USJ 实现完整闭环。在 PC 上执行 ``python -m pip install pyserial`` 安装 ``pyserial``，将下面内容保存为一个文件，关闭其他串口工具，然后使用所选设备端口运行，例如 ``python evmux_smoke.py /dev/ttyACM0``。要上传的 MicroPython 程序已嵌入主机脚本，无需第二个源文件。

.. code-block:: python

   #!/usr/bin/env python3
   import argparse
   import base64
   import json
   import re
   import sys
   import time
   import zlib

   import serial


   DEVICE_SCRIPT = """\
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
   """

   SOF = b"\x1e"
   EOF = b"\x1f"
   HEADER_RE = re.compile(
       rb"EVMUX/1 h=([0-9]+) p=([0-9]+) c=([0-9A-Fa-f]{8})\r\n")
   MAX_METADATA = 64 * 1024
   MAX_PAYLOAD = 16 * 1024 * 1024
   WRITE_CHUNK = 768


   class EvMux:
       def __init__(self, port):
           self.port = port
           self.next_seq = 1
           self.text_tail = bytearray()

       def _read_exact(self, size, deadline):
           data = bytearray()
           while len(data) < size:
               if time.monotonic() >= deadline:
                   raise TimeoutError("timed out while reading EV-MUX frame")
               part = self.port.read(size - len(data))
               if part:
                   data.extend(part)
           return bytes(data)

       def read_frame(self, deadline):
           while time.monotonic() < deadline:
               if self.port.read(1) != SOF:
                   continue

               header = bytearray()
               while len(header) < 80:
                   char = self._read_exact(1, deadline)
                   header.extend(char)
                   if char == b"\n":
                       break
               match = HEADER_RE.fullmatch(bytes(header))
               if match is None:
                   continue

               metadata_len = int(match.group(1))
               payload_len = int(match.group(2))
               expected_crc = int(match.group(3), 16)
               if not 0 < metadata_len <= MAX_METADATA:
                   raise RuntimeError("invalid metadata length: {}".format(metadata_len))
               if not 0 <= payload_len <= MAX_PAYLOAD:
                   raise RuntimeError("invalid payload length: {}".format(payload_len))

               metadata_bytes = self._read_exact(metadata_len, deadline)
               payload = self._read_exact(payload_len, deadline)
               if self._read_exact(1, deadline) != EOF:
                   raise RuntimeError("invalid EV-MUX EOF")
               actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
               if expected_crc and actual_crc != expected_crc:
                   raise RuntimeError("EV-MUX payload CRC mismatch")
               metadata = json.loads(metadata_bytes.decode("utf-8"))
               return metadata, payload
           raise TimeoutError("timed out waiting for EV-MUX frame")

       def send_request(self, channel, method, body):
           seq = self.next_seq
           self.next_seq += 1
           payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
           metadata = {
               "sid": "debug" if channel == "debug.rpc" else "user",
               "seq": seq,
               "channel": channel,
               "type": "req",
               "method": method,
               "encoding": "json",
               "ts_ms": int(time.monotonic() * 1000),
           }
           metadata_bytes = json.dumps(
               metadata, separators=(",", ":")).encode("utf-8")
           crc = zlib.crc32(payload) & 0xFFFFFFFF
           header = "EVMUX/1 h={} p={} c={:08X}\r\n".format(
               len(metadata_bytes), len(payload), crc).encode("ascii")
           self.port.write(SOF + header + metadata_bytes + payload + EOF)
           self.port.flush()
           return seq

       def _observe(self, metadata, payload):
           channel = metadata.get("channel")
           if channel in ("repl.stdout", "repl.stderr", "log.idf"):
               self.text_tail.extend(payload)
               del self.text_tail[:-8192]
               text = payload.decode("utf-8", errors="replace")
               sys.stdout.write("[{}] {}".format(channel, text))
               sys.stdout.flush()

       def rpc(self, channel, method, body, timeout=10):
           seq = self.send_request(channel, method, body)
           deadline = time.monotonic() + timeout
           while True:
               metadata, payload = self.read_frame(deadline)
               self._observe(metadata, payload)
               if (metadata.get("channel") != channel
                       or metadata.get("type") != "rsp"
                       or metadata.get("seq") != seq):
                   continue
               if metadata.get("encoding") != "json":
                   raise RuntimeError("unexpected binary RPC response")
               result = json.loads(payload.decode("utf-8"))
               if not result.get("ok"):
                   raise RuntimeError("{} failed: {}".format(method, result))
               return result

       def wait_for_text(self, marker, timeout=15):
           deadline = time.monotonic() + timeout
           while marker not in self.text_tail:
               metadata, payload = self.read_frame(deadline)
               self._observe(metadata, payload)
           return bytes(self.text_tail)


   def upload_script(mux, path, source):
       content = source.encode("utf-8")
       for offset in range(0, len(content), WRITE_CHUNK):
           chunk = content[offset:offset + WRITE_CHUNK]
           result = mux.rpc("user.rpc", "script.write", {
               "path": path,
               "mode": "overwrite",
               "encoding": "utf-8",
               "contentBase64": base64.b64encode(chunk).decode("ascii"),
               "offset": offset,
               "totalBytes": len(content),
           })
           expected_complete = offset + len(chunk) == len(content)
           if result.get("complete") != expected_complete:
               raise RuntimeError("unexpected script.write completion state")


   def main():
       parser = argparse.ArgumentParser(
           description="ESP-VISION EV-MUX USB/USJ closed-loop smoke test")
       parser.add_argument("port", help="USB-OTG CDC or USB-Serial-JTAG port")
       args = parser.parse_args()

       with serial.Serial(
               args.port, baudrate=115200, timeout=0.2, write_timeout=5) as port:
           try:
               port.dtr = True
           except (OSError, serial.SerialException):
               pass
           port.reset_input_buffer()
           time.sleep(0.1)
           mux = EvMux(port)

           hello = mux.rpc("user.rpc", "hello", {})
           if hello.get("firmware", {}).get("id") != "esp-vision":
               raise RuntimeError("connected device is not ESP-VISION")

           capabilities = mux.rpc("user.rpc", "capabilities", {})
           version = capabilities.get("protocol", {}).get("evMuxVersion")
           if version != 3:
               raise RuntimeError("unsupported EV-MUX version: {}".format(version))
           required = {"user.rpc", "debug.rpc", "repl.stdout"}
           if not required.issubset(set(capabilities.get("channels", []))):
               raise RuntimeError("required EV-MUX channels are unavailable")

           state = mux.rpc("debug.rpc", "transport.state", {})
           print("routes: user={user[route]} debug={debug[route]}".format(**state))

           mux.rpc("user.rpc", "device.control", {"action": "stop"})
           mux.wait_for_text(b">>>", timeout=10)

           path = "/evmux_smoke.py"
           upload_script(mux, path, DEVICE_SCRIPT)
           mux.rpc("user.rpc", "script.run", {"path": path})
           mux.wait_for_text(b"EVTEST:OK", timeout=20)
           print("\nPASS: EV-MUX upload, execution, camera capture, and stdout verified")


   if __name__ == "__main__":
       main()

该测试会将超时、帧格式错误、路由拒绝、RPC 失败响应、未收到摄像头标记或协议版本不支持都视为失败。在产品集成中应保留相同的帧处理和序列关联规则，等待 RPC 时继续读取无关事件，显式处理 ``VM_BUSY`` 和 ``VM_TIMEOUT``，并在观测到预期设备端输出或结果之前绝不报告成功。
