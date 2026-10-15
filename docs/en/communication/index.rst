Communication System and EV-MUX
===============================

:link_to_translation:`zh_CN:[中文]`

This chapter follows the complete ESP-VISION host communication path: MicroPython I/O, ESP-IDF logs and camera data, physical interfaces, session ownership and framing, through the shared Device Core SDK to IDE, Agent, CLI or custom host applications. It describes the current EV-MUX v4 implementation; firmware release, session version and wire format are separate concepts.

.. important::

   ``evMuxVersion:4`` defines session/authorization behavior; ``EVMUX/1`` defines byte framing. v4 still uses EVMUX/1: do not change the header to EVMUX/4. IDE 1.3.0 Device Core accepts only explicitly v4-compatible firmware.

The complete path at a glance
-----------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     default_node_color = "#edf4ff";
     default_fontsize = 12;
     py [label = "MicroPython\nprint / REPL / files"];
     c [label = "C stdio / ESP_LOGx"];
     img [label = "img.flush()\nJPEG preview"];
     hook [label = "HAL / stdio hooks\npreview producer"];
     channel [label = "EV-MUX channels\nuser + debug streams"];
     session [label = "One session owner\nlink + epoch checks"];
     mux [label = "EVMUX/1 frames\nbounded TX queues"];
     io [label = "usj / usb / uart / tcp"];
     transport [label = "Host byte transport\nserial / TCP / WebSocket bridge"];
     core [label = "Device Core\nprotocol / sessions / operations"];
     view [label = "IDE / Agent / CLI / custom tools\nUI / workflows / commands"];
     py -> hook; c -> hook; img -> hook;
     hook -> channel -> session -> mux -> io -> transport -> core -> view;
   }

**Figure 1: Output is classified, ownership-scoped and framed before reaching one physical connection.** Host commands travel in reverse through parser, authorization and REPL/control dispatch. Keep reading because responses, text and images can interleave.

Recommended reading order
-------------------------

.. toctree::
   :maxdepth: 1

   architecture
   interfaces
   sessions
   wire-protocol
   python-files
   preview-debug
   host-integration
   network-discovery
   device-core
   validation

.. list-table:: Find the relevant topic
   :header-rows: 1
   :widths: 40 60

   * - Question
     - Entry point
   * - How do Python print, C printf and IDF logs differ?
     - :doc:`architecture`
   * - What are the two USB interfaces, UART and Wi-Fi/TCP?
     - :doc:`interfaces`
   * - How do competing hosts, disconnects and resets recover?
     - :doc:`sessions`
   * - How do I implement framing and RPC?
     - :doc:`wire-protocol`
   * - How do I input Python, transfer files and run long scripts?
     - :doc:`python-files`
   * - How do preview and debug.capture_frame differ?
     - :doc:`preview-debug`
   * - How do I provision Wi-Fi and handle startup data?
     - :doc:`host-integration`
   * - How do I install Device Core and integrate serial/TCP, files and preview?
     - :doc:`device-core`
   * - How do I prove completion and diagnose failures?
     - :doc:`validation`

Six distinct concepts
---------------------

* **Physical interface / sink**: backend type, for example ``usj`` or ``tcp``.
* **link**: one exact connection with backend, slot and generation; reusing a TCP slot still creates a new link.
* **stream**: internal ``user``/``debug`` routing groups, not eight independent serial ports.
* **channel**: frame semantics, for example ``repl.stdout`` or ``preview.frame``.
* **sessionId**: ownership credential tied to an exact link; it cannot move to another socket.
* **vmEpoch**: MicroPython generation. A surviving session does not preserve old-VM work.

Current implementation boundaries
---------------------------------

There is at most one session owner holding both streams. Other links can discover the device, but cannot run a second independent control session. Acquisition contention and explicit handoff are supported; forced eviction of a valid owner is not.

TCP is a raw STA IPv4 byte server on default port 3232 with at most two accepted links. It is not HTTP, WebSocket or MicroPython WebREPL and does not add authentication/encryption in this version. Browsers use a host-side byte bridge: see :doc:`host-integration`.

ROM download and EV-MUX runtime are separate protocol phases. The MicroPython REPL remains the interpreter console; IDF system console carries early initialization, ROM/bootloader and some fatal output. Raw text at these boundaries is not a valid business frame.
