Device Core host SDK
========================================

:link_to_translation:`zh_CN:[中文]`

``@esp-vision/device-core`` is the UI-independent JavaScript host SDK for ESP-VISION EV-MUX v4 firmware. IDE, Agent, CLI and custom host applications share its byte framing, ownership, keepalive, Python, files and image handling. Its Apache-2.0-licensed source is maintained in ``host/device-core`` in the firmware repository; IDE/Agent applications consume the independently versioned npm package.

.. toctree::
   :maxdepth: 2

   api
   advanced
   integration
   cli

Installation and compatibility
------------------------------------------------------------

.. code-block:: console

   npm install --save-exact @esp-vision/device-core@0.3.0
   # Optional Node LAN discovery adapter:
   npm install multicast-dns

These examples target SDK ``0.3.0``. Install that version once the registry reports it as published; a source checkout, MR or dry run does not publish it. Node.js 22 is the host baseline. Browsers require a bundler/import map and a host-provided Web Serial or binary WebSocket transport. Only EV-MUX v4 is supported; a v3 device must update firmware. Package, firmware and IDE versions are independent.

Architecture and starting point
--------------------------------------------------------------

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: Host applications, Device Core, EV-MUX and firmware

   IDE / Agent / CLI / custom tools → Device Core → EV-MUX → firmware

The same SVG is used by the project README and system architecture chapter. Host applications select devices and provide UI/workflow policy; Device Core owns the shared API, protocol and session state; EV-MUX defines runtime communication with the firmware. Byte adapters supply serial/socket I/O without parsing another protocol stack. Representative APIs in the figure do not replace the full API reference. Discovery and ROM flashing have separate lifecycles; the ROM loader does not carry image writes through EV-MUX.

The root exports only ``createDevice``. It returns a device object; there is no public ``Device`` constructor. Use :doc:`api` for every application method, option, result and event. :doc:`advanced` inventories all other package exports and their public class methods for integration maintainers. These subpaths do not install an Agent plugin or provide IDE UI.

.. code-block:: javascript

   import { createDevice } from '@esp-vision/device-core'

   // transport is supplied by the host; its complete contract is in api.
   const device = createDevice({ transport })
   const off = device.on('python.output', text => console.log(text))
   try {
     await device.connect({ role: 'usb' })
     console.log(await device.python.execute({ source: 'print(1 + 2)' }))
   } finally {
     try { await device.disconnect() } finally { off() }
   }

For complete runnable Node TCP and browser Web Serial adapters, see :doc:`integration`. For packet layouts, routing and ownership, see :doc:`index <../communication/index>`; hardware verification uses the shared entry point described in :doc:`validation <../communication/validation>`.

Licensing
----------------------------------------

The npm package and ESP-VISION contributions follow Apache-2.0, matching the firmware. The package includes LICENSE and NOTICE; NOTICE preserves historical MIT copyrights and permission text. Third-party dependencies retain their own licenses. See :doc:`../license/index` for the project inventory.

Version 0.1.1 adds ``@esp-vision/device-core/node`` adapters and ships runnable unit/hardware checks. This source prepares 0.3.0 with complete-image installation and reliable binary uploads; install it after publication or validate a locally packed tarball first. Version 0.1.0 lacks these exports/check assets. Installed checks: ``npm --prefix node_modules/@esp-vision/device-core test`` and ``npm --prefix node_modules/@esp-vision/device-core run test:hardware -- --help``. A missing or empty suite fails. See :doc:`integration` for application ``discover()`` versus provider ``list()``.
