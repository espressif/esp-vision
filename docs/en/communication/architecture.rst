Architecture, Execution Contexts and Standard I/O
=================================================

:link_to_translation:`zh_CN:[中文]`

System architecture and responsibilities
----------------------------------------

IDE, Agent, CLI and custom tools are peer host applications using the same Device Core package. Core implements host APIs and protocol/session state; EV-MUX is the runtime wire protocol across USB CDC, USJ, UART and TCP; the firmware implements device operations and the MicroPython runtime. The representative API names shown below are an overview, not a complete inventory.

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: Host applications, Device Core, EV-MUX and device firmware

   Shared system diagram; detailed firmware execution paths follow below.

This SVG is shared with the README, system architecture and SDK guides. Discovery and ROM flashing are separate from the EV-MUX runtime path; see :doc:`../architecture/index` for those boundaries. Device scripts execute on the board and do not require a host to remain connected.

Where EV-MUX sits
-----------------

EV-MUX is runtime byte framing and multiplexing. It neither executes Python nor replaces the existing VM. MicroPython retains parsing, compilation, execution, friendly REPL, stdin/stdout and exception printing; ESP-VISION attaches framing and ownership routing at HAL/platform boundaries.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     vm [label = "MicroPython VM\ncompiler / REPL / user code"];
     hal [label = "mp_hal_stdin_rx_chr\nmp_hal_stdout_tx_strn"];
     stdio [label = "ev_stdio\nframing hooks"];
     ctrl [label = "ev_control_transport\nparser / auth / RPC"];
     q [label = "REPL byte queue\nVM request queue"];
     session [label = "ev_session / ev_channel\nowner / epoch / routing"];
     tx [label = "ev_mux\nTX scheduling / frame CRC"];
     backend [label = "ev_transport\nUSB / UART / TCP"];
     vm -> hal -> stdio -> session -> tx -> backend;
     backend -> ctrl -> q -> vm;
     ctrl -> session;
   }

**Figure 2: Interpreter, HAL, control, ownership/routing and physical backends.** stdin/stdout still use the VM; pure transport RPC does not wait for user Python to return.

Output producer, not text content, determines the channel
---------------------------------------------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     python [label = "print / sys.stdout\nREPL prompt / traceback"];
     pystderr [label = "MicroPython sys.stderr\nshared stdio implementation"];
     hal [label = "mp_hal_stdout_tx_strn"];
     cout [label = "C printf / stdout"];
     cerr [label = "C fprintf(stderr, ...)" ];
     log [label = "ESP_LOGx"];
     hout [label = "newlib stdout hook"];
     herr [label = "newlib stderr hook"];
     hlog [label = "esp_log_set_vprintf hook"];
     out [label = "repl.stdout\nUSER stream"];
     err [label = "repl.stderr\nUSER stream"];
     logs [label = "log.idf\nDEBUG stream"];
     python -> hal -> out; pystderr -> hal;
     cout -> hout -> out; cerr -> herr -> err; log -> hlog -> logs;
   }

**Figure 3: Actual output destinations in this build.** Python ``print()`` and C ``printf()`` may share a channel but have different call paths. An "error" word does not automatically redirect text to ``repl.stderr``.

.. list-table:: Output path reference
   :header-rows: 1
   :widths: 24 42 34

   * - Producer
     - Firmware entry point
     - Wire output
   * - Python ``print()``, ``sys.stdout.write()`` and friendly REPL prompts
     - MicroPython print/stdio → ``mp_hal_stdout_tx_strn()`` → ``ev_stdio_write_repl_stdout()``
     - ``repl.stdout``, ``encoding:text``.
   * - Unhandled REPL exceptions and ``KeyboardInterrupt`` tracebacks
     - ``pyexec.c`` uses ``mp_obj_print_exception(&mp_plat_print, ...)`` and HAL stdout.
     - Can appear on ``repl.stdout``; stderr alone is not a failure oracle.
   * - Current MicroPython ``sys.stderr.write()``
     - ``shared/runtime/sys_stdio_mphal.c`` shares ``stdio_write()`` for stdout/stderr; both use cooked HAL stdout.
     - Still ``repl.stdout``, rather than desktop CPython independent stderr.
   * - C ``printf()`` / ``fprintf(stdout, ...)``
     - ``CONFIG_LIBC_NEWLIB`` installs the newlib ``stdout->_write`` hook.
     - ``repl.stdout`` with ``method:c-stdio`` for C stdio provenance.
   * - C ``fprintf(stderr, ...)``
     - newlib ``stderr->_write`` hook.
     - ``repl.stderr``, ``method:c-stdio``.
   * - Normal ESP-IDF ``ESP_LOGI/W/E`` logs
     - ``esp_log_set_vprintf()`` → ``ev_stdio_log_vprintf()``.
     - ``log.idf``, ``method:esp-idf``, debug stream.
   * - Platform logs such as ``esp_vision_debug_printf()``
     - Platform logging calls the EV-MUX IDF-log writer.
     - ``log.idf``; method depends on the producer entry point.
   * - ``img.flush()``
     - Preview producer → JPEG encoder → lossy EV-MUX TX.
     - ``preview.frame`` binary JPEG, bypassing printf.

``encoding:text`` does not imply UTF-8 character or line boundaries at frame ends. Keep an incremental decoder and unfinished-line buffer rather than decoding each payload independently and accepting replacement characters.

.. note::

   This table describes current source. Channel names do not imply language-level stderr behavior. A future dedicated Python stderr HAL path must update the table, clients and protocol tests.

IDF console versus MicroPython console
--------------------------------------

IDF system-console Kconfig selects low-level system destinations. MicroPython port settings enable UART REPL, USJ and TinyUSB CDC. EV-MUX ownership selects the runtime business link. These are three separate layers, not aliases of a single switch.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     boot [label = "ROM / bootloader\nearly IDF logs"];
     idf [label = "IDF system console\nCONFIG_ESP_CONSOLE_*"];
     raw [label = "Raw UART / USJ bytes\nno EV-MUX contract"];
     mp [label = "MicroPython port config\nUART / USJ / TinyUSB"];
     hooks [label = "ev_stdio_init0\ninstall hooks + enable mux"];
     run [label = "Runtime EV-MUX frames\nactive owner link"];
     boot -> idf -> raw;
     mp -> hooks -> run;
   }

**Figure 4: Early system console and runtime EV-MUX have different contracts.** Power-on, ROM and panic paths can emit unframed text; runtime business confirmation still requires valid frames.

``MICROPY_HW_ESP_USB_SERIAL_JTAG`` controls the ESP-VISION/MicroPython USJ path. Some target/version defaults may still send IDF console output to USJ when it is disabled. Enabled MicroPython USJ initialization explicitly enables peripheral clocks/internal PHY instead of relying on console initialization.

``sensor.evmux(False)`` is a runtime diagnostic switch restoring non-framed text paths. ``ESP_VISION_EV_MUX_DEFAULT_ENABLED`` is local to ``ev_stdio.c`` and is not currently connected to ``boardconfig.h``; defining it there does not disable power-on mux. Disabling framing is not v3 fallback negotiation.

Execution responsibilities
--------------------------

.. list-table:: Execution contexts
   :header-rows: 1
   :widths: 25 42 33

   * - Context
     - Work
     - Relationship to user Python
   * - transport task
     - Read links, frame/authorize ingress, handle discovery, lease/release, network transactions and pure-C transport stats.
     - These controls remain serviceable during user execution; there is no second VM.
   * - VM task
     - Friendly REPL, user code and VM RPC touching VFS/objects/camera.
     - A long script can occupy this task. Queues are bounded; stop it before work requiring an available VM.
   * - TX task
     - Deliver frames, handle partial writes/backpressure/priority and link loss.
     - Producer data is copied or has a controlled lifetime, not a dangling VM temporary pointer.
   * - Network worker / TCP manager
     - Wi-Fi setup, provisioning transactions, connectivity/listener maintenance and slot reclamation.
     - Shares underlying Python WLAN; ordinary soft reset does not repeatedly initialize the driver.
   * - USB CDC callbacks / restart timer
     - Record DTR/RTS and defer download/reset work.
     - Do not tear down USB in its control callback; this is separate from EV-MUX acquire.

``file.write`` runs in the transport task and writes native VFS under session/VM generation and storage drain checks; it does not enter the interpreter queue. Legacy ``script.write``, ``script.run``, ``debug.capture_frame``, ``debug.capture_screen`` and object-related ``debug.info`` enter the VM queue; ``scope:transport.stats`` is the exception. Two ring-array slots provide one usable pending request. Queue expiry is 5000 ms; busy/expired work reports ``VM_BUSY``/``VM_TIMEOUT``.

LCD screenshots read the display layer's last submitted RGB565 frame and encode JPEG in the same VM task. They do not invoke ``sensor`` or the camera, change displayed content, subscribe to preview or read back a physical panel. Execution is serialized with display/soft-reset cleanup to avoid reading freed frame buffers.

``repl.signal`` Ctrl-C schedules ``KeyboardInterrupt`` when interpreter interrupts are armed; otherwise it enters REPL editing input. It is not FreeRTOS task preemption and cannot guarantee interruption of a non-returning native call.

Soft-reset and background communication lifetime
------------------------------------------------

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     Host -> Transport [label = "soft-reset request / REPL command"];
     Transport -> VM [label = "stop input admission; advance epoch"];
     VM -> VM [label = "touch / display\ncamera / VFS\nGC cleanup"];
     VM -> VM [label = "new VM\nboot.py\nmain.py"];
     VM -> Transport [label = "vmReady=true; event/hello"];
     Host -> Transport [label = "session.keepalive"];
     Host <-- Transport [label = "same owner; newer epoch; vmReady"];
     Host -> VM [label = "current-epoch REPL barrier"];
     Host <-- VM [label = "new complete marker"];
   }

**Figure 5: Soft reset advances VM generation while links/network/session can survive.** Stop old admission before touch/display/preview/camera cleanup. Epoch advances at reset start, not VM readiness; wait for readiness and real REPL evidence.

Queues, logging failures and absence of raw fallback
----------------------------------------------------

Normal queues allow 16 KiB/8 frames per serial endpoint, with two TCP slots sharing a budget. Control reserves another 4 KiB/2 frames. REPL staging uses 512 bytes and an approximately 5 ms coalescing window; preview keeps the latest pending frame: see :doc:`preview-debug`.

While mux is enabled, submission/TX failure does not rebroadcast the same Python/C text through raw serial. Hooks guard recursion; error paths use counters instead of more traffic. IDF vprintf has a 512-byte temporary buffer and rejects oversized formatted output with a drop/error, rather than claiming complete delivery.

Early console, mux-disabled paths and low-level panic/ROM are different lifecycle boundaries, not send-failure fallback.

Source entry points
-------------------

``platform/ev_stdio.c`` and ``overlay/micropython/ports/esp32/mphalport.c`` adapt I/O; ``ev_control_transport.c`` dispatches ingress; ``ev_session.c``/``ev_channel.c`` manage ownership/routes; ``ev_mux.c`` manages framing/snapshots/queues/TX; ``ev_vm_output.c`` manages VM output scope; ``main.c`` manages startup/reset. Python stderr is implemented in ``lib/micropython/shared/runtime/sys_stdio_mphal.c``.
