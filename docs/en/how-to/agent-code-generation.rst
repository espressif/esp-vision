Agent Code Generation and EV-MUX Closed-Loop Test
==================================================

:link_to_translation:`zh_CN:[中文]`

This guide is normative for AI agents that generate ESP-VISION application code. Treat the target as an ESP32 microcontroller running the ESP-VISION build of MicroPython v1.28.0, not as a PC running CPython. Use only APIs present in this knowledge pack, enabled for the selected board, or demonstrated by a repository example; never assume that a desktop standard-library module, ``pip`` package, native wheel, operating-system service, subprocess, or PC filesystem is available on the device.

Dependency Rules
----------------

The default ESP-VISION firmware cannot dynamically install dependencies from ``requirements.txt`` and does not include device-side ``mip``. Generated target code must not invoke ``pip``, parse ``requirements.txt``, call ``mip.install()``, or silently depend on an unverified third-party package. If a required module is not in the knowledge pack, either rewrite the solution with supported modules or explicitly state that it must be copied to ``/lib`` with a host tool, precompiled as compatible ``.mpy``, frozen into custom firmware, or implemented as a native module before the script can run.

Host-side tooling is separate from target code. A PC utility may use CPython packages such as ``pyserial``, but label that code as host-only and never import those packages from the ESP-VISION script.

All-in-One Output
-----------------

Unless the user asks for a package or a multi-file architecture, return one self-contained ``main.py`` for the device. Put supported imports, constants, initialization, application logic, the main loop or bounded test, error handling, and cleanup in that file. Avoid helper modules that the user would have to create separately. Models and other binary assets may remain separate files, but list their exact device paths and deployment prerequisites.

Prefer bounded, observable verification code before an infinite product loop. Print a unique success marker only after the camera, model, storage, network, or other requested operation has really succeeded. The following target-side smoke test is an all-in-one MicroPython script and prints ``EVTEST:OK`` only after capturing a frame:

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

USB-OTG CDC and USJ Transport Contract
---------------------------------------

ESP-VISION carries the same EV-MUX protocol over USB-OTG CDC (sink ``cdc``) and USB-Serial-JTAG (sink ``usj``). Open exactly one device port for a test. Opening the CDC port with DTR asserted makes CDC the automatic route; otherwise supported boards fall back to USJ. Do not open both ports or leave another serial monitor attached while testing. USB baud rate is not part of the protocol, although host serial APIs still require a placeholder baud value.

Every frame is ``0x1e`` followed by ``EVMUX/1 h=<metadata_len> p=<payload_len> c=<crc32>\r\n``, exactly ``h`` bytes of UTF-8 JSON metadata, exactly ``p`` payload bytes, and a final ``0x1f``. Lengths, not delimiters, define the metadata and payload boundaries. Device frames carry the payload CRC32; a host command may send a zero CRC, although computing it is recommended. A receiver must resynchronize on ``0x1e``, validate lengths and CRC, then dispatch every frame by ``metadata.channel`` rather than by USB port or arrival order.

The minimum closed loop is: open the selected port; exchange ``hello`` and ``capabilities``; verify ``firmware.id == "esp-vision"`` and the negotiated ``evMuxVersion``; query ``transport.state``; stop the current script to reach the REPL; upload one complete device script with ``script.write``; start it with ``script.run``; and require an expected marker on ``repl.stdout``. A ``script.run`` response only means the script was queued, so it is not a successful application test by itself.

.. seqdiag::

   seqdiag {
     "host"; "device transport"; "MicroPython VM";

     "host" -> "device transport" [label = "hello / capabilities"];
     "device transport" --> "host" [label = "identity / protocol features"];
     "host" -> "device transport" [label = "transport.state / stop"];
     "host" -> "MicroPython VM" [label = "script.write chunks"];
     "host" -> "MicroPython VM" [label = "script.run"];
     "MicroPython VM" --> "host" [label = "repl.stdout: EVTEST:OK"];
   }

The following host-only CPython script implements that complete loop for either USB-OTG CDC or USJ. Install ``pyserial`` on the PC with ``python -m pip install pyserial``, save this as one file, close other serial tools, and run it with the selected device port, for example ``python evmux_smoke.py /dev/ttyACM0``. The uploaded MicroPython program is embedded in the host script, so no second source file is required.

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

This test deliberately treats a timeout, malformed frame, rejected route, negative RPC response, missing camera marker, or unsupported protocol version as a failure. For product integration, retain the same framing and correlation rules, keep reading unrelated events while awaiting an RPC sequence, handle ``VM_BUSY`` and ``VM_TIMEOUT`` explicitly, and never report success until the expected device-side output or result has been observed.
