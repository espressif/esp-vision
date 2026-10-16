# Repository guide for coding agents

ESP-VISION contains the MicroPython vision firmware for ESP32-P4/S3/S31 and the independently versioned `@esp-vision/device-core` host SDK. IDE, Agent, CLI and custom hosts share Device Core above EV-MUX v4; IDE/Agent applications live in separate repositories and consume the npm package.

## Start here

| Task | Implementation / entry point | Reference |
| --- | --- | --- |
| Architecture and boundaries | `platform/`, `modules/`, `host/` | [Architecture](docs/en/architecture/index.rst) |
| Device Core installation and API | [Package](host/device-core/README.md), `host/device-core/src/index.js` | [SDK guide](docs/en/device-core/index.rst), [API](docs/en/device-core/api.rst), [advanced exports](docs/en/device-core/advanced.rst) |
| Host CLI and device discovery | `host/device-core/src/cli.js`, `node_inventory.js`, `network_discovery*.js` | [CLI](docs/en/device-core/cli.rst) |
| EV-MUX, ownership and Python | `platform/ev_*.c`, `host/device-core/src/{evmux,evrepl,device_session}.js` | [Communication reference](docs/en/communication/index.rst) |
| Network and identity | `platform/network_service.*`, `platform/device_identity.*` | [Discovery](docs/en/communication/network-discovery.rst) |
| Camera and LCD capture | `platform/{camera,display,preview}.c`, `boards/<BOARD>/camera.c` | [Preview/capture](docs/en/communication/preview-debug.rst) |
| Firmware builds and boards | `idf_ext.py`, `micropython.cmake`, `boards/<BOARD>/` | Board configuration and `.gitlab/ci/` |
| Hardware acceptance | `host/device-core/tests/hardware-check.mjs`, `tools/ci/flash_and_test.py` | [Validation](docs/en/communication/validation.rst) |
| Licensing | `LICENSE`, source headers, `host/device-core/NOTICE` | [License inventory](docs/en/license/index.rst) |

Documentation lives in `docs/en/` and `docs/zh_CN/`; update both translations for changed behavior. Device Core's complete documentation belongs in this documentation center; its package README links there.

## Implementation rules

- The application SDK root exports `createDevice()`. Follow the grouped API in the API reference; protocol helpers have explicit package subpaths. Do not duplicate framing, session ownership or device workflows in IDE/Agent clients or new test scripts.
- Public APIs and advertised capabilities must work end to end. No placeholder methods or simulated success; unsupported hardware/firmware returns explicit errors. EV-MUX v3 clients must use matching older firmware/tools, not compatibility branches inside the v4 SDK.
- Transport adapters carry bytes. Device Core owns the host protocol/session; firmware owns device arbitration. Discovery and ROM flashing have separate lifecycles. USB-OTG and USB-Serial-JTAG are distinct interfaces; passive enumeration must not reset devices.
- Keep Python module names OpenMV-compatible (`sensor`, `image`, `display`, `image.ImageIO`). Bindings in `modules/` handle object conversion; heavy work belongs in `platform/` or pure-C components. Use ESP-VISION names outside the upstream imlib headers/sources.
- Handle allocation, DMA, PSRAM/cache, storage ownership and USB failure paths explicitly. Reuse lifetime-scoped buffers without reserving unused camera/LCD buffers at boot; release resources on failure, reconfiguration and shutdown.
- Prefer `overlay/micropython/` and `USER_C_MODULES` over modifying the MicroPython submodule. `components/imlib/` holds pure-C vision code; per-board camera backends override the platform defaults. Examples, Python stubs and models live in `example/`, `stubs/` and `models/`.

## Build and verify

Use repository-root `idf.py --board <BOARD> ...` through `idf_ext.py`, with a supported ESP-IDF environment on `PATH`. It checks the pinned `lib/micropython` revision, creates a clean build copy, applies `overlay/micropython/`, and projects `boards/<BOARD>/port/` into it. Do not create a standalone root IDF application or edit generated build/Board Manager files. Board definitions and CI configuration determine supported board/IDF combinations.

During experiments, compile only the board being tested, using an explicit build directory under `/home/libo/project/.esp-vision/`. For example, replace the board and directory below with the actual test target:

```sh
idf.py --board ESP32_P4X_EYE -B /home/libo/project/.esp-vision/build/ESP32_P4X_EYE build
```

Prefer CI for the final firmware matrix after the feature is complete. SDK-only changes use `ESP_VISION_DEVICE_CORE_ONLY=1`; `ESP_VISION_BUILD_FIRMWARE=1` requests firmware validation. See CI rules for targeted filters.

- SDK changes: use Node.js 22+, install dependencies in `host/device-core` with `npm install --ignore-scripts --no-audit --no-fund --package-lock=false`, then run `npm run release:check` (tests, schema, package and publish dry run).
- Firmware host checks: `python -m unittest discover -s tools/tests -p 'test_*.py' -v`.
- Hardware checks: from `host/device-core`, start with `npm run test:hardware -- --help`; reuse this public-SDK entry point. CI flashing delegates to it through `tools/ci/flash_and_test.py`.
- Run applicable `pre-commit` hooks. Respect the upstream/generated exclusions in `.pre-commit-config.yaml`; never rewrite upstream license headers. Syntax-check changed stubs and validate bilingual documentation/MCP references when relevant.
- Report tested boards, interfaces and commit/pipeline coverage accurately. Historical success does not validate a newer commit; canceled jobs are neither passing nor failing tests. Never format device storage to hide a test failure.

## Changes and releases

- Every committed change updates root `CHANGELOG.md` under `[Unreleased]`, grouped by Added/Changed/Fixed/Removed. SDK release changes also update its package changelog/version as appropriate; npm versions are independent of firmware versions. Git tags identify firmware/documentation releases, not SDK releases.
- Device Core publication is controlled by `.gitlab/ci/device-core.yml`: relevant non-master changes run publish dry runs; actual npm publication uses protected master credentials. Do not expose tokens or commit secrets.
- Use meaningful branch prefixes (`feat/`, `fix/`, `docs/`, etc.), never `codex/`. For Espressif GitLab API access use `tk_gitlab` to load the token without printing it.
- Keep temporary worktrees, builds, caches, logs and scratch scripts under `/home/libo/project/` (ESP-VISION: `.esp-vision/`); set `TMPDIR`, `TMP` and `TEMP` accordingly. Do not commit generated output, local device paths or unrelated workspace changes.
- Markdown prose uses one line per paragraph/list item; do not hard-wrap it.

## Licensing

Original firmware and Device Core code use Apache-2.0; preserve the SDK's historical MIT notices in `NOTICE`. Upstream MicroPython, OpenMV bindings/imlib, submodules and registry components retain their own per-file licenses and headers. Consult the linked license inventory instead of inferring a license from a directory; record new dependencies there. Keep `OMV_NO_GPL=1` and excluded GPL paths disabled. If third-party terms are unclear, ask before incorporating that source.
