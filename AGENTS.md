# Repository guide for coding agents

ESP-VISION contains the MicroPython vision firmware for ESP32-P4/S3/S31 and the independently versioned `@esp-vision/device-core` host SDK. IDE, Agent, CLI and custom hosts share Device Core above EV-MUX v4; IDE/Agent applications live in separate repositories and consume the npm package.

Keep this file focused on shared repository agreements and navigation. Put implemented contracts, supported limits and maintained usage instructions in the linked component guides. Keep personal settings and work-in-progress records outside the checkout.

## Repository content boundary

This shared repository contains reviewed project deliverables: source, reproducible tests and fixtures, shared build/CI configuration, public package metadata, and maintained documentation of implemented behavior. A document is not a deliverable merely because an agent created it, a task finished, or it was placed under `docs/`.

- Never commit personal configuration, credentials, local environment/device settings, agent memory or transcripts, reasoning traces, temporary plans, design proposals, investigation diaries, handoff notes, experiment output or raw validation reports. Do not create `specs/` for agent work or relocate these records into documentation to make them committable.
- Before creating a file, decide its audience and lifecycle. Keep working material in the external workspace selected by `ESP_VISION_WORK_DIR`; put review evidence and unresolved follow-ups in the MR/issue or CI artifacts. Extract only verified, maintained contracts into the existing documentation structure, updating both languages where applicable. Keep reusable test code and sanitized fixtures, not one-off execution results.
- Keep user-specific paths, shell aliases, credentials and agent preferences in local configuration outside the checkout. Shared instructions must use configurable paths and portable commands. Shipped plugin manifests and Skills under `host/esp-vision-plugin/` are product source, not permission to commit personal agent settings elsewhere.
- Before committing, inspect `git status --short`, `git diff --cached --name-status` and the complete staged diff. Stage explicit task files; exclude unrelated work and audit each added file against this boundary. Update the changelog and run `python tools/ci/check_repository_hygiene.py`. The check inspects the index, including force-added ignored files, and also runs in pre-commit and CI.
- Ignore rules and the path check prevent known artifact locations; they cannot classify arbitrary prose or detect every secret. Review content regardless of its filename. Never use `git add -f` or a new exception to bypass this boundary for working material. Preserve existing uncommitted work outside the checkout before removing it; do not delete another task's files as cleanup.

## Start here

| Task | Implementation / entry point | Reference |
| --- | --- | --- |
| Architecture and boundaries | `platform/`, `modules/`, `host/` | [Architecture](docs/en/architecture/index.rst) |
| Device Core installation and API | [Package](host/device-core/README.md), `host/device-core/src/index.js` | [SDK guide](docs/en/device-core/index.rst), [API](docs/en/device-core/api.rst), [advanced exports](docs/en/device-core/advanced.rst) |
| Host CLI and device discovery | `host/device-core/src/cli.js`, `node_inventory.js`, `network_discovery*.js` | [CLI](docs/en/device-core/cli.rst) |
| EV-MUX, ownership and Python | `platform/ev_*.c`, `host/device-core/src/{evmux,evrepl,device_session}.js` | [Communication reference](docs/en/communication/index.rst) |
| Network and identity | `platform/network_service.*`, `platform/device_identity.*` | [Discovery](docs/en/communication/network-discovery.rst) |
| Camera and LCD capture | `platform/{camera,display,preview}.c`, `boards/<BOARD>/camera.c` | [Preview/capture](docs/en/communication/preview-debug.rst) |
| Firmware builds, application-only flashing and boards | `idf_ext.py`, `micropython.cmake`, `boards/<BOARD>/` | [IDF commands](docs/en/get-started/index.rst#idf-app-flash), board configuration and `.gitlab/ci/` |
| Mosaico Iris layout and application-only flashing | `idf_ext.py`, `boards/ESP32_S31_MOSAICO/port/partitions-16MiB-irisfw.csv` | [Iris flashing guide](docs/en/how-to/publish-firmware.rst#iris-app-flash), [中文](docs/zh_CN/how-to/publish-firmware.rst#iris-app-flash) |
| Hardware acceptance | `host/device-core/tests/hardware-check.mjs`, `tools/ci/flash_and_test.py` | [Validation](docs/en/communication/validation.rst) |
| Host plugin packaging and marketplace acceptance | `host/esp-vision-plugin/`, `tools/ci/mcp/` | [Plugin maintainer guide](host/esp-vision-plugin/README.md) |
| Licensing | `LICENSE`, source headers, `host/device-core/NOTICE` | [License inventory](docs/en/license/index.rst) |

Documentation lives in `docs/en/` and `docs/zh_CN/`; update both translations for changed behavior. Device Core's complete documentation belongs in this documentation center; its package README links there.

## Implementation rules

- The application SDK root exports `createDevice()`. Follow the grouped API in the API reference; protocol helpers have explicit package subpaths. Do not duplicate framing, session ownership or device workflows in IDE/Agent clients or new test scripts.
- Public APIs and advertised capabilities must work end to end. No placeholder methods or simulated success; unsupported hardware/firmware returns explicit errors. EV-MUX v3 clients must use matching older firmware/tools, not compatibility branches inside the v4 SDK.
- Transport adapters carry bytes. Device Core owns the host protocol/session; firmware owns device arbitration. Discovery and ROM flashing have separate lifecycles. USB-OTG and USB-Serial-JTAG are distinct interfaces; passive enumeration must not reset devices.
- Keep Python module names OpenMV-compatible (`sensor`, `image`, `display`, `image.ImageIO`). Bindings in `modules/` handle object conversion; heavy work belongs in `platform/` or pure-C components. Use ESP-VISION names outside the upstream imlib headers/sources.
- Handle allocation, DMA, PSRAM/cache, storage ownership and USB failure paths explicitly. Reuse lifetime-scoped buffers without reserving unused camera/LCD buffers at boot; release resources on failure, reconfiguration and shutdown.
- Storage faults must keep the REPL/control transports alive. Automatic initialization is authorized only on exact `FR_NO_FILESYSTEM` by independent board policies: `ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM` defaults to `1`, and `ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM` defaults to `0`. Enabled policies may erase damaged or foreign data; data-preserving products must keep the relevant policy at `0`. Generic I/O/internal errors and missing NVS metadata do not authorize formatting; `storage.retry()` remains mount-only. Explicit recovery requires user confirmation and writer/handle checks. Follow [Storage and data protection](docs/en/storage/index.rst) for backup and Flash/SD interruption limits.
- Prefer `overlay/micropython/` and `USER_C_MODULES` over modifying the MicroPython submodule. `components/imlib/` holds pure-C vision code; per-board camera backends override the platform defaults. Examples, Python stubs and models live in `example/`, `stubs/` and `models/`.

## Build and verify

Use repository-root `idf.py --board <BOARD> ...` through `idf_ext.py`, with a supported ESP-IDF environment on `PATH`. It checks the pinned `lib/micropython` revision, creates a clean build copy, applies `overlay/micropython/`, and projects `boards/<BOARD>/port/` into it. Do not create a standalone root IDF application or edit generated build/Board Manager files. Board definitions and CI configuration determine supported board/IDF combinations.

During experiments, compile only the board being tested, using an explicit build directory under a writable workspace outside the checkout. Set `ESP_VISION_WORK_DIR` to choose that workspace; the example defaults to `esp-vision` under the system temporary directory. Replace the board below with the actual test target:

```sh
export ESP_VISION_WORK_DIR="${ESP_VISION_WORK_DIR:-${TMPDIR:-/tmp}/esp-vision}"
mkdir -p "$ESP_VISION_WORK_DIR/tmp"
export TMPDIR="$ESP_VISION_WORK_DIR/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR"
idf.py --board ESP32_P4X_EYE -B "$ESP_VISION_WORK_DIR/build/ESP32_P4X_EYE" build
```

Prefer CI for the final firmware matrix after the feature is complete. SDK-only changes use `ESP_VISION_DEVICE_CORE_ONLY=1`; `ESP_VISION_BUILD_FIRMWARE=1` requests firmware validation. See CI rules for targeted filters.

- Root `idf.py --board <BOARD> app` and `app-flash` are available for all supported board/IDF combinations, including ordinary firmware with Iris disabled. `app` builds the application; `app-flash` writes only the application at the current build's partition-derived offset. The installed layout and selected boot application must already match; do not assume the Iris offset for other builds. Only the automatic `flash` to `app-flash` selection is Iris-specific.
- For a Mosaico already using the Iris layout and booting `main_app`, follow the linked Iris flashing guide: keep `ESP_VISION_ENABLE_IRISFW=ON` in a separate build directory and use root `idf.py app-flash`. Its application offset is `0x210000`; Iris `flash` selects the same app-only target. Use the ROM download interface, not Vibe Mode. Do not substitute ordinary factory-layout images or replay the full `flash_args` manifest, which would overwrite retained system metadata. App-only flashing neither installs the layout nor changes the selected boot application; native Iris installation acceptance is a separate test.
- SDK changes: use Node.js 22+, install dependencies in `host/device-core` with `npm install --ignore-scripts --no-audit --no-fund --package-lock=false`, then run `npm run release:check` (tests, schema, package and publish dry run).
- Firmware host checks: `python -m unittest discover -s tools/tests -p 'test_*.py' -v`.
- Hardware checks: from `host/device-core`, start with `npm run test:hardware -- --help`; reuse this public-SDK entry point. CI flashing delegates to it through `tools/ci/flash_and_test.py`.
- Development boards may be reflashed directly. Do not automatically back up or restore installed firmware, or add whole-flash/protected-region preservation reads before or after flashing; these operations are too slow for routine development. Keep normal programming verification appropriate to the task. Back up or restore firmware only when the user explicitly requests it.
- Run applicable `pre-commit` hooks. Respect the upstream/generated exclusions in `.pre-commit-config.yaml`; never rewrite upstream license headers. Syntax-check changed stubs and validate bilingual documentation/MCP references when relevant.
- Report tested boards, interfaces and commit/pipeline coverage accurately. Historical success does not validate a newer commit; canceled jobs are neither passing nor failing tests. Never format device storage to hide a test failure.

## Changes and releases

- Every committed change updates root `CHANGELOG.md` under `[Unreleased]`, grouped by Added/Changed/Fixed/Removed. SDK release changes also update its package changelog/version as appropriate; npm versions are independent of firmware versions. Git tags identify firmware/documentation releases, not SDK releases.
- Device Core publication is controlled by `.gitlab/ci/device-core.yml`: relevant non-master changes run publish dry runs; actual npm publication uses protected master credentials. Do not expose tokens or commit secrets.
- Use meaningful branch prefixes (`feat/`, `fix/`, `docs/`, etc.), never `codex/`. Load GitLab credentials through the developer's local credential setup or CI secret store without printing them; do not require a personal shell alias in shared instructions.
- Keep temporary worktrees, builds, caches, logs and scratch scripts under the writable `ESP_VISION_WORK_DIR` workspace described above; set `TMPDIR`, `TMP` and `TEMP` accordingly. Use a persistent directory outside the checkout when artifacts must survive system temporary-directory cleanup. Do not commit generated output, local device paths or unrelated workspace changes.
- Markdown prose uses one line per paragraph/list item; do not hard-wrap it.

## Licensing

Original firmware and Device Core code use Apache-2.0; preserve the SDK's historical MIT notices in `NOTICE`. Upstream MicroPython, OpenMV bindings/imlib, submodules and registry components retain their own per-file licenses and headers. Consult the linked license inventory instead of inferring a license from a directory; record new dependencies there. Keep `OMV_NO_GPL=1` and excluded GPL paths disabled. If third-party terms are unclear, ask before incorporating that source.
