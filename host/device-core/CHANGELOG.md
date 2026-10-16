# Changelog

## 0.1.1

### Fixed

- Expose `tcpTransport` and `serialTransport` through `@esp-vision/device-core/node`.
- Include unit tests, hardware checks, fixtures and npm script targets in the tarball. Fail when no unit tests are found; validate imports, discovery and runnable checks from an isolated installed package.
- Clarify `device.discover()` versus the provider's `list()` contract and document the shipped Node adapters and installed test commands.

## 0.1.0 — Initial public release

### Added

- Added browser-safe and optional Node LAN discovery interfaces, TTL-aware DNS-SD collection, and selection-time device identity validation before EV-MUX acquisition.

- Standalone, UI-independent EV-MUX v4 sessions, strict ownership/epoch filtering, framed REPL, files, execution, Wi-Fi controls and device-action contracts.
- Verified firmware-flash adapter with injected ROM loader, image/security/partition checks, complete readback and masked P4 download-latch reset.
- Public npm metadata, protected default-branch publishing, isolated tarball checks and verified/idempotent release retries. The package has no Agent/IDE runtime dependency or bundle; the official firmware index uses a small TOML parser, and Node discovery optionally uses multicast-dns.

- Default official firmware discovery reads dl.espressif.com config and per-version metadata; legacy unqualified releases remain visible without enabling unsafe writes. CLI uses --port and a built-in esptool process adapter, removing the user-supplied JavaScript loader requirement.

- Ship `esp-vision-device` management CLI: unified `inspect` passive/probed wired/LAN queries matching the SDK method name, platform-aware USB serial filtering with `--all-ports` for native UART/Bluetooth, numbered/grouped endpoints, strict configuration/control and catalog/version firmware commands. Reuse Core facade and one cross-platform Python serial/TCP adapter in both CLI and hardware checks.

- Added `device.firmware.check()` and `device.firmware.update()` for complete board-version listing, exact-version selection, ordered release catalogs, board selection, validated application download, exclusive ROM handoff, readback verification and explicit reset/reconnection results.
- Publish bilingual complete API documentation in the ESP-VISION documentation center from repository `docs/{en,zh_CN}/device-core/`; the npm README links to its online HTML pages.

### Changed

- License the npm package and ESP-VISION contributions under Apache-2.0, matching the firmware; retain historical MIT copyright/permission notices in the distributed NOTICE file.

- Moved the SDK source and release checks from the IDE repository to `host/device-core` in the ESP-VISION firmware repository; npm name and independently versioned package contract are unchanged.

- **Breaking:** the package root exports only `createDevice()`; the device class stays internal. Use the grouped application API for discovery, connection, inspection, configuration, control, Python, files, capture and events. Existing protocol, action-contract and ROM-flashing integrations remain on their explicit subpaths; the README includes a migration table.
- Consolidated network mutations into `configure()` and reset/interrupt/download-mode requests into `control()`, with strict parameters, cancellation and shared-operation coordination.
- Unified file access under `files` and execution under `python`, with acknowledged upload progress, directory-scoped enumeration and execution of existing paths without rewriting their contents.

### Fixed

- Preserve split mDNS address announcements during cache flush, reclaim expired cache entries, bound packet retention, and close discovery sockets when cancellation occurs during setup.
- Soft reset requires a newer ready epoch and a real REPL barrier within one deadline; caller cancellation reaches all stages and device-action adapters.
- Bounded file reads reject incomplete/changing content; serial startup drain, TCP frame deadlines and keepalive recovery follow the v4 contract.

- Align discovery output by Unicode display width instead of tabs; fall back to aligned, wrapped device blocks on narrow terminals without truncating identifiers.

- Generate script completion inside the executing statement so `input()` receives only user input; retain output arriving in the same frame as completion.
- Return binary image results correlated to the requesting sequence instead of a shared last-image slot. LCD capture uses the dedicated `debug.capture_screen` capability and never falls back to the camera; this refactor does not implement the planned high-throughput binary firmware protocol.
- Clean up subscriptions, pending work and transports on connection failure or session loss, and isolate event-listener exceptions from protocol delivery.

The earlier v3 extraction existed only inside repository development history; it was not an npm release of this package. Version 0.1.0 is the first public v4-only package.
