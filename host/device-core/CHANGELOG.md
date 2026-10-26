# Changelog

## Unreleased

### Fixed

- Invalidate the old lease after acknowledged or uncertain hard reset, block concurrent renegotiation, and serialize cleanup with reset so stale release failures cannot mask its result. Retain live leases after confirmed reset denials and discard explicit ownership rejections.

## 0.5.1

### Fixed

- Make packaged execution-deadline regressions deterministic with a controlled clock, including expiry before VM entry, and keep installation guidance independent of temporary publication state.

## 0.5.0

### Changed

- Move the host management CLI to the standalone `@esp-vision/device-cli` package and keep passive Node inventory helpers available from `@esp-vision/device-core/node`.

### Fixed

- Detach started Python runs from connection-lifetime cancellation after VM entry, so normal disconnect releases ownership without sending a remote stop; retain caller AbortSignal cancellation.

- Wait for a private VM-entry sentinel before resolving detached Python startup, with bounded startup waits and session-isolated output.

- Add validated `connect({ probeRepl: false })` for non-interrupting native RPC access and use it for metadata inventory probes; retain the existing active connection default.

- Make aggregate deadlines optional by default for completed Python runs and file reads while retaining bounded REPL exchanges and per-chunk file-read limits.
- Keep wait-mode small-source execution on the REPL until its completion sentinel arrives, without imposing a fixed program-completion timeout or requiring device storage.

## 0.4.2

### Fixed

- Route browser ROM reset calls through the active flash stub after upload, preserving its two-byte status decoder during P4 watchdog register writes; report reset failure diagnostics without invalidating successful firmware verification.

## 0.4.1

### Fixed

- Fail camera mode checks on allocation, timeout and unexpected errors instead of silently classifying those modes as unsupported; inspect optional storage state only when advertised.
- Keep hardware-check fixtures in bounded RAM-only Python calls, independent of filesystem availability. Add expected board/firmware and required-check validation, report storage mount state, and reject explicit file acceptance on unmounted Flash without formatting.

## 0.4.0

### Added

- Reuse browser cache only for hash-pinned release manifests and complete-BIN sources; retry a fetch-stage TypeError once with no-store for cached CORS recovery, while hash/HTTP/metadata failures and legacy app-only downloads retain strict failure/cache behavior.

- Export immutable official release-manifest readers with publication/version/board/hash pinning and bounded, source-constrained validation of firmware, website, models and MCP snapshots; model files are unique by path while a group may share one slug, and normalized paths cannot escape the pinned source commit.
- Support application updates from the qualified complete BIN through `image.source`, verifying the whole container and embedded segments before extracting the application; preserve explicit full installation and legacy app-only descriptors.

- Export passive `openBrowserRom({ port, logger?, signal? })` at `/browser` with a lazily loaded, pinned tasmota browser backend; share verified Core firmware install/update workflows without host-maintained ROM patches.
- Validate the real browser bundle, P4/S31 stub assets, adapter cleanup/cancellation and isolated package imports.

## 0.3.0

### Added

- Export the passive built-in `openNodeRom({ port, python?, signal? })` adapter from `/node` so Node SDK hosts and the CLI share the same ROM implementation.

- Explicit `mode: 'install'` on firmware check/update and CLI `--mode install` for blank-device ROM installation over USJ/UART, using qualified complete images with bootloader/partition/application hashes and flash-capacity checks. Default application-only updates retain their existing data-preservation contract; no automatic fallback or TCP OTA is introduced.

### Fixed

- Allow up to 151 seconds of bounded metadata propagation backoff after npm accepts publication, with ten uncached verification reads and no repeated publish.

## 0.2.0

### Changed

- File and large-script uploads use one reliable binary `file.write` engine with 16 KiB blocks, integrity validation and cancellation cleanup; no Base64/JSON or REPL upload fallback. Public `files.write` and Python API signatures stay unchanged. Uploads require matching firmware capability `file.write.binary`; older v4 devices can still be inspected but must update firmware before upload.

### Fixed

- Verify newly published tarballs using bounded uncached registry reads instead of a single immediate npm view; retry transient metadata propagation/network failures without repeating publication, while failing immediately on access denial or integrity mismatch.

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
