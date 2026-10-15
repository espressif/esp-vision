Agent Code Generation Guide
===========================

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

Device Communication and Acceptance
-----------------------------------

Agent is a host application alongside IDE and CLI; use Device Core for device interaction rather than another EV-MUX client. The shared host/Core/protocol/firmware relationship is in :doc:`../architecture/index`. Use :doc:`../communication/index` for acquisition, upload, execution and application evidence. The complete host-only camera closed-loop example is in :doc:`../communication/validation`. Handshake, upload acknowledgement, echo and REPL prompts do not prove application success.
