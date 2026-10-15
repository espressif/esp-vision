LAN discovery protocol and board defaults
=========================================

:link_to_translation:`zh_CN:[中文]`

.. note::

   The implementation advertises an ESP-VISION DNS-SD service and provides Device Core discovery adapters for Node and the browser bridge. Firmware build/hardware qualification remains a separate release gate. Existing hello discovery operates only after a transport is connected.

Lightweight protocol selection
------------------------------

Use mDNS/DNS-SD for the first implementation. It needs no central server and reuses the existing ``espressif/mdns: ~1.1.0`` dependency shared by IDF 5.5, 6.0 and 6.1 manifests. DNS-SD uses PTR for enumeration, SRV for host/port, TXT for metadata and A records for the current IPv4 address.

.. list-table:: Implementation tradeoffs
   :header-rows: 1
   :widths: 20 40 40

   * - Protocol
     - Cost and benefit
     - Decision
   * - mDNS/DNS-SD
     - Existing component and host tooling; caches and naming conflict handling are standardized.
     - Preferred; register one service with bounded TXT metadata.
   * - Custom UDP query/reply
     - A smaller dedicated responder is possible, but requires a new port, parser, retry/cache and multi-interface rules.
     - Reserve for a measured resource constraint; do not ship two discovery protocols initially.
   * - SSDP/UPnP
     - Adds another discovery format and, for full UPnP integration, device descriptions.
     - No advantage for this existing EV-MUX endpoint.

The pinned mDNS 1.1.0 Kconfig defaults to a 4096-byte task stack. This is not total heap usage. Reuse its single existing instance rather than creating another discovery task/socket. Measure image size, free/minimum internal heap and stack watermark before and after service registration; no measured RAM delta is available yet. Avoid continuous application-level beacons; use the component query, announcement and TTL behavior.

Service contract
----------------

* Browse ``_esp-vision._tcp.local`` on the local link using mDNS UDP port 5353.
* Hostname: ``esp-vision-<full-board-name>-<last-6-mac-hex>-<firmware-version>.local``; normalize board/version to lowercase DNS labels with hyphens. Version is the firmware build version, never the IDE version. Names stay within 63 bytes; long versions retain a prefix plus a deterministic eight-digit build digest, while TXT retains the original version. Main-chip MAC is used on every board. The hostname label (without ``.local``) and full device ID are identical in hello events/responses, capabilities and device debug information over USB, USJ, UART and TCP.
* Instance display name: board name plus a short identity suffix. SRV advertises the actual ``ESP_VISION_TCP_PORT`` (default 3232); do not duplicate the port in TXT.
* TXT keys: ``txtvers=1``, ``id=<device-id>``, ``board=<board>``, ``fw=<firmware-version>``, ``evmux=4``, ``hostname=<canonical-hostname>``. Keep the total encoded TXT payload within 256 bytes and validate lengths before registration.
* Reuse the Launcher device identity derived from the main chip eFuse MAC, including on Hosted Wi-Fi boards. Do not substitute the companion radio MAC, IP, boot ID or session ID. If stable identity is unavailable, report discovery unavailable instead of inventing a random permanent identity.
* Never advertise passwords, session credentials or a periodically refreshed owner state. Discovery metadata is untrusted; it is not authentication.

Firmware lifecycle and Python coexistence
-----------------------------------------

The platform network service owns mDNS initialization and coordinates with ``overlay/micropython/ports/esp32/network_wlan.c``, which currently initializes mDNS and sets the MicroPython hostname. Do not retain two independent initialization flags. Python mDNS queries reuse the shared instance. ``network.hostname()`` returns the same canonical name; assigning another hostname raises ValueError, preserving one firmware-defined identity. DHCP and the Python port use the same name. Soft reset does not regenerate it. ESP-IDF network-interface hostnames are aligned to the 63-byte DNS limit; DHCP option space is set to 255 bytes to accommodate that label alongside other options.

Register the service only after STA has IPv4 and the EV-MUX TCP listener has successfully bound. Withdraw it when the listener closes, Wi-Fi is disabled or STA disconnects; advertise the new address after reconnect. An unexpected power loss is handled by host TTL expiry. Soft reset must not duplicate the service or stop the platform discovery responder. mDNS failure is observable but must not stop working TCP/USB access. The shared network sdkconfig limits mDNS to the STA interface, matching the TCP bind address; AP/Ethernet interfaces do not advertise this endpoint.

Host and Device Core integration
--------------------------------

Device Core owns the discovery-result contract and cancellation/timeout semantics; an injected environment adapter performs network browsing. Node hosts can use a DNS-SD adapter directly. Browser IDE obtains results through the local Node bridge because ordinary web pages cannot browse multicast DNS. This does not introduce Agent dependencies into Core.

Results include stable device ID, display name, board, firmware, EV-MUX version, hostname, IPv4 addresses, TCP port and interface identity. Cache by stable ID while retaining interface/address candidates; remove expired addresses and retain valid alternatives. Hostname alone is not a device identity. Use bounded queries, support AbortSignal and release browse resources on cancellation.

Browsing does not connect to every TCP endpoint or acquire ownership. After user selection, Core connects and issues non-acquiring hello to verify v4, stable device identity and current occupancy, then explicitly acquires the session. Both acquiring and non-acquiring hello responses include stable ``deviceId`` and ``hostname`` fields. When ``expectedDevice`` is supplied to ``DeviceSession.negotiate``, a non-acquiring hello verifies these fields before requesting ownership. Report an occupied session; takeover continues to follow the existing explicit policy.

The bridge only browses and forwards bytes; EV-MUX remains in Core. Discovery HTTP/control endpoints require the same exact-origin and loopback policy as the byte bridge. Select a resolved candidate through a bridge-issued opaque handle, revalidate its cached address/port, and retain an explicit fixed-target manual mode; do not expose unrestricted browser-selected TCP forwarding.

IDE places refresh and discovered-device selection in the existing network connection section. Keep manual IP/hostname input for networks with multicast filtering, client isolation or separate VLANs. Local Network Access permission does not itself provide DNS-SD enumeration.

Default-enabled Wi-Fi board policy
----------------------------------

Wi-Fi-equipped boards must enable WLAN, ``ESP_VISION_ENABLE_BACKGROUND_NETWORK=1`` and ``ESP_VISION_ENABLE_TCP_TRANSPORT=1`` by default. The current seven Wi-Fi configurations already satisfy this policy: ``ESP32_P4X_EYE`` and ``ESP32_P4X_FUNCTION_EV_BOARD`` use Hosted Wi-Fi; ``ESP32_S3_EYE``, ``AtomS3R-M12``, ``ESP32_S31_CHATBOT``, ``ESP32_S31_KORVO`` and ``ESP32_S31_MOSAICO`` use native Wi-Fi. The two Hosted boards include ``sdkconfig.p4_wifi_common``.

``ESP32_P4X_VISION`` currently disables WLAN and has no Hosted Wi-Fi configuration. P4 alone has no native Wi-Fi; confirm an attached radio and its board wiring before enabling this port. ``TEMPLATE`` also remains hardware-dependent. Do not copy Hosted SDIO/reset pins from another board.

Default-enabled means the firmware service is available at startup without a Python script. Empty factory SSID remains unconfigured, valid saved profiles reconnect, and saved disabled/forgotten intent is preserved. TCP starts listening only when network conditions permit. Discovery follows the TCP listener lifecycle and the same default policy.

Implementation and acceptance
-----------------------------

1. Firmware: unify mDNS lifecycle, register/withdraw the service and expose stable identity in non-acquiring hello.
2. Core: define adapter/results, cancellation and selection-time identity checks; keep Node and browser discovery separate from the EV-MUX parser.
3. IDE: extend the local bridge and connection panel; retain manual addressing.
4. Verify multiple boards, occupied sessions without takeover, multiple host interfaces, DHCP address changes, disable/re-enable, abrupt power loss/TTL, soft reset, discovery cancellation and filtered multicast fallback. Run the full firmware matrix in CI and measure resource deltas on native and Hosted hardware.

Device Core discovery examples
------------------------------

Node installs the optional multicast-dns adapter dependency; browsers use the existing local bridge. Start the bridge with ``npm run network:bridge`` (no fixed IP required), then refresh/select a device in the IDE network panel. Candidate handles expire after at most 30 seconds; refresh before reconnecting an expired selection.

.. code-block:: console

   npm install @esp-vision/device-core multicast-dns

.. code-block:: javascript

   import { createNodeDiscovery } from '@esp-vision/device-core/discovery/node';
   const devices = await createNodeDiscovery().list({ timeout: 3000 });
   // Optional: createNodeDiscovery({ interfaceAddress: '<local-interface-ipv4>' })

.. code-block:: javascript

   import { createBridgeDiscovery } from '@esp-vision/device-core/discovery';
   const devices = await createBridgeDiscovery('ws://127.0.0.1:8766/device').list();
   // Connect the chosen connectionUrl, then:
   // await session.negotiate({ role: 'tcp', expectedDevice: devices[0] });

Protocol references
-------------------

* `Multicast DNS, RFC 6762 <https://www.rfc-editor.org/rfc/rfc6762.html>`_
* `DNS-Based Service Discovery, RFC 6763 <https://www.rfc-editor.org/rfc/rfc6763.html>`_
* `Espressif mDNS 1.1.0 Kconfig <https://github.com/espressif/esp-protocols/blob/mdns-v1.1.0/components/mdns/Kconfig>`_
