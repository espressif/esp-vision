# Changelog

All notable changes to ESP-VISION are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/); each released version corresponds to a git tag. Unreleased changes accumulate at the top and are folded into the next tag at release time.

## [Unreleased]

### Added

- Add SC101IOT DVP camera support on ESP32_S31_MOSAICO alongside OV3640.

- Add ESP32_P4X_VISION hardware CI.

### Changed

- Update the ESP-IDF 5.5, 6.0 and 6.1 component manifests to esp_video 2.5.0 and esp_h264 1.4.*.

- Prepare Device Core 0.7.1 as an independent npm patch release for faster complete installation and ROM-to-runtime firmware detection; retain the existing firmware and plugin release lifecycle.

- Speed up complete firmware installation by trusting checked, partition-bounded FFAT erase commands without erased-range reads, and reuse official esptool whole-write MD5 verification receipts in Node/browser adapters instead of transferring firmware back to the host. Keep SHA-256 qualification before writing and full readback for custom adapters without receipts.

### Fixed

- Keep Mosaico SC101IOT framing consistent across QQVGA, QVGA and VGA with hardware scaling, a wider field of view and no black borders.

- Fix ESP32_P4X_VISION hardware CI to match the board's supported capabilities.

- Drive the partial ROM-packet deadline regression with a controlled clock so CI scheduling delays cannot consume its successful receive budget.

- Classify an occupied EV-MUX session as busy during firmware identification; USB descriptors, ROM responses and timeouts do not establish the presence or absence of ESP-VISION firmware.
- Keep passive ROM enumeration non-opening; active firmware detection verifies the selected ROM MAC/security, attempts normal SPI boot once, then rebinds only its matching physical USB endpoint before the ESP-VISION handshake. Preserve unknown startup outcomes and require download re-entry when no runtime returns.

## [2026.11.06]

This release adds S31 USB-OTG automatic ROM entry, verified complete installation and addressed Flash erase, negotiated UART communication, safe camera observation, shared USB/TCP preview, storage initialization policies, and validated camera/ISP/AI behavior. Firmware and npm package versions remain independent: application SDK Device Core 0.7.0, Device CLI 0.1.1 with bundled Core 0.5.1, Knowledge MCP 0.1.2 and Plugin 0.1.1.

### Added

- Exercise P4/S31 production download control handling with the Core serial opening policy and the complete official USB-JTAG sequence, including repeated/coalesced requests and the final 50 ms deferral; separately model why a Linux opening with cached DTR/RTS both disabled can request a normal reset, without changing firmware policy or claiming measured kernel signals.

- Use the S31 ROM digital-system reset for software download entry after later-registered shutdown cleanup, allowing ROM to reinitialize retained USB-OTG state; register the restart hook before USB/machine/network startup independently of the CDC timer, fail explicitly on registration errors, and retain normal S31/P4/S3 reset behavior.

- Enable S31 Korvo/Mosaico USB-OTG automatic ROM entry through deferred CDC DTR/RTS control and machine.bootloader(), using the masked LP download latch; keep Chatbot on its connected USB Serial/JTAG interface. Normal return after programming requires the matching S31 latch handling in Device Core 0.7.0 or an equivalent host.

- Add browser ROM factory-MAC identity binding to Device Core 0.7.0 through the pinned official chip reader, exposing Node-compatible `chipMac` and optional prevalidated `expectedMac`; reject initial or recovered identity changes before stub startup, erase or write, with browser-bundle and same-model-device regressions.

- Default unlabeled automatically mounted FAT volumes and MSC LUN names to `ESP_FLASH` / `ESP_SDCARD`, preserving custom labels, board-specific USB MSC interface strings and writer ownership without formatting; add a bilingual USB interface/configuration and disk-naming chapter explaining Windows volume names, and align storage acceptance with the explicit initialization policy.
- Add independent board policies `ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM` (default `1`) and `ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM` (default `0`) to initialize internal `ffat` or opt-in SD only on `FR_NO_FILESYSTEM`; keep mount-only retries, writer/handle guards and raw SD FatFs diagnostics. Flash initialization may erase unrecognized/foreign contents; SD preserves them by default.
- Add the Python `/flash` alias and matching native uploads, independent default-on board policies for Flash/SD automatic mounting, and documented fixed USB MSC slots for optional volumes.
- Add real-frame ISP regressions for single CCM writes, explicit white-balance preservation and IPA ownership, plus LSC switching with CCM write bursts during camera/canvas/LCD preview, DMA/internal/PSRAM heap measurements and cleanup/restart recovery.
- Add the `storage` diagnostic, mount-retry and explicitly confirmed Flash recovery API, with a bilingual chapter defining media/filesystem ownership, data-loss prevention, verified off-device backups, restore procedures and user-controlled failure handling.

- Added read-only TCP `preview.frame` subscriptions for two simultaneous observers alongside an independent control session, bounded shared JPEG fan-out, a Python/ffplay viewer and targeted native/hardware acceptance checks; TCP frame segments use scatter/gather writes without extra application copies.
- Added native composite CDC/MSC/UVC output on USB-device boards, a default ESP-VISION logo independent of sensor capture, Python fixed/live frame APIs, and shared JPEG fan-out from `Image.flush()` to UVC and EV-MUX.
- Add a bounded camera/real-AI/LCD/WiFi MJPEG stress example with one shared full-panel RGB565 canvas, per-frame hashes, configurable ESPDet/TFLite models, health/heap/latency counters, slow-client deadlines and cooperative cleanup.
- Publish independent camera observation frames after successful application captures and advertise `debug.capture_frame.observed.v1`; Core 0.6.0 requires this capability before camera capture, including advanced RPC/binding entry points, so older firmware cannot overwrite an application's live Image through the unsafe legacy operation.
- Add negotiated runtime UART speeds in EV-MUX v4, testing 2,000,000, 1,500,000, 921,600, 460,800 and 115,200 bps in order with bounded bidirectional checks, physical transmit drain, tentative-switch rollback and session-release recovery.
- Add shared browser and Node serial retuning in Device Core and reproducible dual-interface runtime acceptance checks for S31 Korvo file transfers, camera preview and soft reset.

- Add a Cursor Plugin export with the shared ESP-VISION Skill, official Knowledge MCP and complete local CLI/license closure, source provenance, deterministic ZIP/checksum output, documented local import and Git-based marketplace submission, and CI candidate artifacts.

- Prepare a separate QoderWork plugin candidate ZIP with host-specific metadata, role instructions, official Knowledge MCP setup, the unchanged local CLI/license closure, and explicit marketplace-validation gaps and linked maintainer build instructions.

- Export source-bound WorkBuddy Expert test candidates as MR/branch CI artifacts; publish fixed tag-version Expert ZIPs and checksums to the Generic Package Registry with GitLab Release attachment links only after protected firmware release qualification, independently of npm publication.
- Add a reproducible Node.js WorkBuddy Expert ZIP builder with bilingual Agent/Skill metadata, the original CLI and license closure, Knowledge MCP dependency, portable resource paths, and a 512-pixel official-brand avatar; keep the legacy WorkBuddy installer and published npm version unchanged.
- Added a repository security policy defining ESP-VISION's development and DIY scope, exclusion from the Espressif Bug Bounty Program, private security reporting, and the trusted-network requirements of unauthenticated EV-MUX TCP debugging, with English and Chinese README notices and communication-guide warnings.
- Add strict ESP-DL inference hardware acceptance with a SHA-256-verified ESPDet model in a temporary read-only RAM VFS, real camera inference, repeatability/output checks, transfer/latency measurements and identity-scoped cleanup after uncertain mount results; persistent Flash/SD acceptance remains independent.

### Changed

- Prepare the 2026.11.06 firmware release and immutable Knowledge MCP 0.1.2 / Plugin 0.1.1 npm archives. Bind both host package manifests to published Device CLI 0.1.1 and its declared bundled Core 0.5.1; keep the separately published application SDK Core 0.7.0 and the standalone CLI lifecycle independent.

- Document the public address-based ROM erase/write/read primitives separately from firmware installation policy, including the portable one-write-per-session contract and integer JEDEC ID; clarify that Core 0.7.0 adds region erase and complete-install deletion semantics, while partition-address changes are handled by supplied metadata.

- Prepare Device Core 0.7.0: explicit complete installations erase and verify the internal `ffat` partition derived from the authenticated image's real partition table, after image/layout/chip/security/capacity checks; add bounded browser/Node region erase, failure/cancellation regressions and bilingual deletion/adapter documentation. Application updates retain their existing layout and files; SD cards and unrelated Flash ranges are not erased.

- State the ESP32-P4 v3.1–v3.99 silicon range in the development-board list and firmware notes; v1.3 users must compile and validate their own matching bootloader and application.
- Keep the Function EV hardware CI gate on its eighteen explicitly qualified checks, including ESP-DL inference; retain the two scene-dependent ISP checks for separate acceptance without weakening strict pass requirements or public all-check selection, and verify that CI actually selects every required issue regression.

- Document the OV2710 scoped build adaptation and verbatim driver fixture provenance in both README translations.
- Make agent workspace paths configurable.

- Record that development-board flashing does not require automatic firmware backup, restoration or whole-flash preservation reads; retain ordinary programming verification and reserve backup/restore for explicit requests.
- Deliver the Node ROM recovery and bounded readback corrections as Device Core 0.6.1 with matching action-schema metadata after the published 0.6.0 release, retaining independent CLI and Plugin release lifecycles.
- Make Device Core package/release checks invoke npm's JavaScript entry point on Windows without a shell, and accept CRLF checkout line endings in generated-schema verification without accepting content changes.

- Clarify that validated compressed JPEG input is a borrowed view owned by its source Image and that asynchronous consumers must copy the returned buffer.

- Use one queued camera buffer on ESP32_S31_KORVO while retaining the DVP controller's independent backup frame, saving 1,843,200 bytes for the SC101IOT 1280×720 YUYV input alongside AI and the RGB LCD.

- Publish new Device Core and CLI versions automatically from protected default-branch pipelines; publish Plugin and Knowledge npm packages on qualified firmware tags with matching Core/CLI minor ranges, reproducible resolved patches and a registry-verified bundled CLI; enforce patch/minor/major version steps.

- Build WorkBuddy, QoderWork and Cursor release ZIPs in GitHub Actions and attach verified tag artifacts to GitHub Releases, creating a draft when needed and preserving existing assets; retain npm publication in GitLab and remove its automatic WorkBuddy ZIP publisher.

- Refine the WorkBuddy Chinese listing to the 50-character limit and record the rechecked official template inconsistencies while retaining Engineering classification and matching initial prompts.

- Keep five-host plugin metadata, Knowledge endpoint, Skill/device workflow and CLI/license closure authoritative in shared sources; derive publisher metadata and listings from the shared identity, generate compatibility manifests and reuse one export engine for WorkBuddy Expert, QoderWork and Cursor.

- Document durable QoderWork release attachments from verified tag artifacts and label CI downloads for both plugin platforms.

- Use the requested Qoder display name “ESP-Vision 嵌入式 AI 视觉开发” consistently in the manifest, role and package documentation.

- Verify that actual QoderWork exports retain the shared troubleshooting Q&A, explicit Skill reference, consent-based GitHub feedback and private security-reporting link.

- Document the source-bound host dependency preparation for QoderWork candidates so a same-version historical npm SDK cannot be mistaken for the CI-tested source payload.
- Add shared plugin troubleshooting Q&A for Windows Python/pyserial selection (GitHub #17), host authorization blocks, failed discovery and port ownership, with explicit Skill references and consistent Expert and legacy WorkBuddy reference adaptation.
- Route functional plugin feedback through duplicate-checked, template-based English GitHub Issues drafts with user-approved submission and attachments; keep Knowledge MCP read-only and security reports private.

- Keep root AGENTS.md focused on repository-wide agreements and navigation; move WorkBuddy platform constraints to the linked plugin maintainer guide.

- Recheck the current WorkBuddy Expert/Skill specifications against the actual archive, document contradictory sample ZIP fields and remaining host/avatar acceptance, and extend archive checks for Agent/Skill metadata and MCP icon resolution.

- Name the WorkBuddy Expert ESP-Vision AI视觉 (ESP-Vision AI), describe its ESP32-based vision development scope in the listing, Agent and Skill, and include ESP32 among the visible discovery tags.
- Clarify plugin payload identity and safe recovery of old development copies, WorkBuddy's manual activation contract, the intentional Knowledge MCP/local CLI split, and board/firmware-specific ISP and live-capture verification in the packaged Skill and support reference.
- Clarified the active IPA JSON control scope, OV2710 sensor-side automatic exposure/gain, sensor-native AE/AWB versus manual ISP ownership, driver readback versus captured frames, and LSC device-error recovery in the sensor documentation and stubs.
- Simplify host architecture nodes to component names and npm package names, remove operational annotations, and compact the shared diagram so the firmware layers remain prominent.
- Refresh the shared dark/light/adaptive architecture diagrams and bilingual README/architecture guides for Plugin, standalone Device CLI, public Device Core and the official online Knowledge MCP; show all four public npm versions and package responsibilities in the READMEs.

### Fixed

- Install Python and pyserial in the tag-only Plugin packager, exercise that same packager on relevant branches and merge requests, and isolate the no-tag publication regression from inherited release-tag variables while testing both branch and tag environments.

- Bind Expert archive identity checks to the independently versioned npm manifest instead of a historical hard-coded version, and retain the Core notice copied from the verified published CLI artifact used for Plugin packaging.

- Complete serial startup frames that cross the initial 300 ms drain window within the parser's existing progress/total budgets, without extending the fixed deadline for later frames or accepting old sessions; reject parser receive timeouts even after they clear the buffer, and clean up waits on cancellation or close. Add byte-count/timing diagnostics and regressions for split/coalesced byte boundaries, timeout budgets and injected legacy parsers.

- Clear and verify the S31 ROM download latch at its own target address after Device Core programming, preserving unrelated bits before normal reset; cover successful return and failed latch verification for both S31 and P4. Firmware-side S31 USB-OTG entry is delivered separately.

- Correct the storage documentation heading widths for strict English and Chinese Sphinx builds.
- Correct the ISP validation heading underlines in both languages so strict Sphinx HTML and PDF builds pass.

- Quiesce and acknowledge OV2710 statistics timer deletion before P4 EYE camera teardown releases SCCB, retain callback context on bounded shutdown failures, and report shutdown errors without retrying uncertain native ownership.
- Keep storage failures nonfatal with actionable status/upload errors, preserve unrecognized media when automatic initialization is disabled, require confirmation for explicit Flash recovery, persist Flash format intent/completion without replaying interrupted transactions, and keep unmounted volumes unavailable to USB MSC.

- Diagnose failed CLI serial setup with bounded checks of the selected Python/pyserial environment after cleanup, preserve busy/access-denied and runtime I/O distinctions, and document Windows `--python py` setup without automatic installation or retries. Clarify camera versus LCD capture in real help and bilingual CLI documentation, including firmware-dependent camera drawing semantics.
- Make ESP-DL HIL uploads acknowledge the nonce and committed offset, safely replay only the latest identical block after at most two timeout recoveries, and require semantic mount/cleanup acknowledgements instead of treating printed Python exceptions as success; retain existing deadlines and strict model/inference assertions.

- Reuse an existing connection to the requested SSID in the MJPEG and Cloud AI examples, reject a different connected network, and wait within the existing retry budget for managed WiFi connection attempts or EBUSY instead of repeatedly disabling STA.
- Fix the English WebREPL support heading underline so HTML and PDF documentation builds pass the Sphinx warning gate.

- Grow EV-MUX owner-preview, shared-observer and large-reply buffers to bounded capacities based on actual payloads instead of preallocating 256 KiB per slot during availability checks; preserve active/shared frame storage and retain pending bytes and deadlines on allocation failure.
- Hand off the complete aligned hardware JPEG output buffer from CPU cache before IDF writes its header and starts payload DMA, preventing dirty allocation cache lines from later overwriting encoded bytes; fail explicitly before DMA when cache synchronization fails and keep CPU fallback memory separate.
- Accept native Windows CRLF startup lines in the direct Python serial-failure regression rather than timing out before its assertions.
- Validate every CRC-checked EV-MUX JPEG retained by the UVC diagnostic with the shared strict decoder, removing its missing-io failure and rejecting entropy loss accepted by Pillow alone.
- Clarify that the EV-MUX runtime does not support traditional MicroPython WebREPL, correct the bilingual build/API claims and example path, and report an ordinary error before changing Wi-Fi or opening a listener. Network control continues through Device Core TCP session authorization.
- Preserve bounded Node serial read/write diagnostics and Python bridge exit status without logging transferred bytes, while distinguishing unexpected failures from intentional disconnect and cancellation.

- Abort native UVC frames after failed or short USB ISO transfers, send a bounded error/end marker where possible, and release the source through a failure callback so missing payloads cannot be followed by a misleading frame tail. Keep the managed TinyUSB source intact and compile a checksum-pinned MIT adaptation with fault regressions.
- Require complete JPEG decoding for every retained UVC frame in both standalone and concurrent TCP acceptance, including a regression that removes one 510-byte middle payload while preserving SOI/EOI.
- Require strict FFmpeg decoding and an exact decoded-frame count for retained UVC/MJPEG acceptance, rejecting missing entropy payloads that Pillow can silently conceal; fail explicitly when FFmpeg is unavailable and install it for host CI checks.

- Preserve runtime UART negotiation, image ownership and complete-JPEG validation alongside native UVC/TCP preview; reject partial compressed frames before fan-out and retain bounded CPU encoding when composite USB leaves insufficient internal DMA memory for the hardware JPEG engine.
- Keep TCP observer progress and absolute frame deadlines independent of runtime UART baud changes, avoiding premature disconnection of a progressing observer in the combined transport scheduler.

- Read the stored JPEG length through its const image view, keeping compressed-frame pass-through compatible with strict IDF 6.1 compiler checks.

- Allocate EV-MUX RX parsers only for real endpoint slots to preserve internal RAM as TCP capacity grows; treat temporary TCP send memory exhaustion as bounded backpressure, and close stalled sockets outside the shared I/O lock with a short FIN allocation timeout.
- Delay the initial UVC isochronous frame until the host has time to queue receive transfers, avoiding a truncated JPEG during stream startup.
- Reserve sufficient VFS driver slots on ESP32_P4X_EYE for ISP/camera/codec devices alongside storage and consoles, avoiding camera initialization failures at the default eight-slot limit.
- Fall back to software JPEG encoding when the P4 hardware encoder exhausts DEFAULT-capable internal DMA memory, and pass through already-compressed image frames for UVC and EV-MUX output.
- Wait up to five seconds for the same native USB device to reappear in its verified runtime after a read-only ROM initialization fails; preserve identity and ambiguity checks, cancellation and the single-handoff boundary without retrying writes.
- Apply the requested Node ROM read packet budget to the external esptool reader, defaulting to one outstanding 1 KiB packet while retaining upstream ACK/MD5 verification and whole-image SHA-256 readback. Require the tested esptool 5.6.0 reader before connecting, restoring temporary adapter settings on every outcome and rejecting unsupported layouts before Flash writes.

- Bind Node native USB flashing to the selected device across runtime/ROM re-enumeration, including the UVC composite runtime: verify its EV-MUX identity before one bootloader request, wait for the same USB MAC/location, and verify the actual ROM MAC before allowing writes. Wait for the explicitly selected path to settle instead of guessing another port; allow one read-only initialization recovery only after observing that same device back in its runtime.
- Read the six-byte factory MAC explicitly for USB serials, runtime identities, launcher state and `machine.unique_id()`. Avoid eight-byte EUI-64 writes into six-byte buffers on ESP32-S31 and preserve factory MAC identity on P4/S3.

- Fix bootloader handoff cleanup so a stale EV-MUX release cannot mask a submitted or uncertain transition; drain the Node serial bridge before closing it, retain genuine pre-submission failures, and expose bounded ROM initialization diagnostics in CLI JSON.
- Synchronize the Node ROM loader without reset before probing a known UART bridge or USB Serial/JTAG reset fallback; keep native and unknown USB ports free of UART reset pulses, recognize an existing stub, and close failed connections before retrying.

- Reject ESP-DL models whose native tensor arenas did not initialize before constructing image preprocessing or exposing inference, releasing the partial model and VFS model buffer before raising Python `MemoryError`; exercise the shared constructor and NanoTrack cleanup with C++ fault regressions in CI.
- Give an accepted TCP hard-reset response a bounded graceful-close attempt on its exact detached socket before restarting; retain explicit queue, transmission and local-close failures and require the host's actual RPC ACK for acceptance.
- Keep framebuffer allocation bookkeeping on the native heap so MicroPython collection and VM heap rebuilding cannot reclaim live buffer-stack nodes during buffered BMP/PNM file writes; retain explicit allocation failures and scoped cleanup.
- Preserve ImageIO permanent memory-stream buffers during ordinary framebuffer scope cleanup, stopping before unlinking or freeing a permanent data node while retaining explicit stream-close and VM-reset cleanup.
- Install Pillow in CI host checks so independent full JPEG decoding regressions execute instead of being skipped.
- Shrink CPU JPEG scratch directly into an independent result cache, avoiding a second allocation while the largest native block is occupied; release the scratch explicitly if shrinking fails and never reuse CPU result memory for hardware DMA.
- Fall back to bounded CPU JPEG compression when hardware scratch allocation runs out of memory before DMA starts, choosing scratch capacity from the largest native free block with a 1 MiB maximum, explicit overflow failures and allocation-stage diagnostics while retaining exact result caches, source pixels, requested quality and strict validation for live LCD captures alongside AI and HTTP streaming.
- Initialize imlib software JPEG quantization tables on a fresh first encode at quality 100, preserving the requested quality and rejecting truncated output.
- Preserve explicit SPI LCD direct-PSRAM DMA settings in Board Manager generated configurations on ESP-IDF 5.5.5+ and 6.0.2+ backports, avoiding unintended internal-DMA bounce allocation on supported P4 builds while retaining older-header compatibility.
- Recover hardware JPEG input and output buffers after allocation failures instead of caching a positive capacity with a null buffer and rejecting later smaller previews.

- Run the npm release-policy test fixture through Node on Windows and POSIX, retaining publication and integrity assertions without relying on shell scripts or PATH separators.
- Reject invalid JPEG encoder lengths and malformed image structures before camera, screen or preview transmission; validate received JPEG structure and dimensions in shared Device Core, reject the matching capture with `INVALID_IMAGE`, and discard invalid previews without exposing image callbacks.
- Record passive-capture hardware check phases and raw JPEG responses before validation, and document the neutral startup fixture needed to test reset invalidation.
- Correct the Chinese runtime UART section heading width so strict Sphinx HTML and PDF builds accept it.
- Keep passive camera capture from initializing or acquiring the sensor, changing application pixels, or borrowing external Python buffers; invalidate observed frames on camera reconfiguration and VM reset, report missing frames/allocation failures explicitly, and reject camera mutations from Python worker tasks.
- Allow source-bound host CI staging to replace Core-only transitive dependencies when its ROM adapter changes, while preserving the registry lock and rejecting changes to unrelated or shared host dependencies.
- Use the official pinned esptool-js browser adapter with compressed flashing, verified UART speed selection, native USB ROM synchronization, checked streaming readback digests and unchanged full-image SHA-256 verification; normalize 24-bit flash IDs and retain qualified recovery boundaries.

- Verify the Qoder upload ZIP retains Windows device-path shell guidance so native Node receives `/models` unchanged.

- Normalize QoderWork payload permissions to executable 755 or non-executable 644, preserving file bytes while making candidate ZIPs reproducible across local and CI umasks.

- Build QoderWork CI candidates with strictly validated GitLab source SHA metadata without requiring Git in the slim Node image; retain local Git provenance and cover successful/no-output failure paths with an empty executable search path.
- Reject invalid CLI device paths before connecting, diagnose Windows host paths caused by shell conversion, and document preservation of device paths in Git Bash/MSYS and PowerShell.

- Normalize WorkBuddy Expert payload permissions to 644/755 while retaining executable files, so source umask differences cannot change checked ZIP bytes or tag retry identity.

- Enforce the ESP32-based Chinese listing scope, 40–50-character description, keywords and visible bilingual tags in both Expert packaging and checked release ZIP consumption, with regressions for missing scope and out-of-range descriptions.
- Enforce WorkBuddy's 15-character localized Expert name limit before packaging and release publication, use compact ESP-Vision AI names consistently, and record the name/version limits in persistent maintainer guidance.
- Keep WorkBuddy Expert candidate versions within the platform's 20-character limit by using the pipeline ID without a SHA suffix, retain full source provenance in the archive and receipt, and reject oversized local/release versions before artifact output or publication.
- Reported the supported manual ISP BF level range (`2..20`) and the `enable=False` disable option when an invalid level is supplied.
- Validate MR-built Device Core artifacts against their temporary consumer lock and installed source bytes, preserving the published registry lock and all unrelated dependency pins instead of requiring changed Core source to match an immutable published tarball.
- Include ESP32_S31_MOSAICO in the generated website board catalog with its official V1.2 board image and valid target-support documentation, preserving the firmware mapping from the shared board registry.
- Invalidate the previous EV-MUX session after an acknowledged or uncertain hard-reset RPC so USB Serial/JTAG cleanup cannot replace the reset result with a stale `session.release` timeout; serialize reset against session release and renegotiation while preserving confirmed reset rejections and ordinary release failures.

## [2026.10.23]

This release fixes publication and deployment reliability after 2026.10.21. Firmware runtime behavior is unchanged. Firmware tags and npm package versions are independent.

### Changed

- Prepare Knowledge MCP 0.1.1 for the 2026.10.23 knowledge snapshot and source provenance. Device Core remains at 0.5.1; Device CLI and Plugin remain at 0.1.0.

### Fixed

- Validate npm archives locally with `npm pack --dry-run` so CI can qualify already published versions without attempting publication. Real publication still enforces immutable versions and exact archive integrity.
- Mirror qualified release source commits to GitHub before DL validates model-source URLs, independently of manual npm publication; enforce the dependency order with a CI regression check.
- Keep the Knowledge image repository allowlist in website deployment configuration and remove registry expansion from the upstream bridge. Verify the producer/consumer contract against the independently configured repository.

## [2026.10.21]

This release improves camera/LCD coexistence and live application control, and introduces the standalone Device CLI, official plugin and npm-delivered Knowledge MCP. The published host packages are Device Core 0.5.1, Device CLI 0.1.0, Knowledge MCP 0.1.0 and Plugin 0.1.0.

### Added

- Add mandatory P4 target regressions for camera/LCD coexistence, shutdown/restart, unsupported Image keywords, caller-owned snapshot buffers and the configured allocation policy.
- Expose passive Node device inventory and adapter asset URLs through public Device Core APIs.
- Add the standalone `@esp-vision/device-cli` package with Python execution, binary file operations, camera/screen capture, sequential batches and bounded observation; provide versioned JSON results, streamed events and cancellation.
- Add `@esp-vision/plugin` with a fixed bundled Device CLI, one capability-oriented Skill and the official Knowledge HTTP MCP. Support verified native Codex/Claude installation, upgrades and failure recovery, and prepare WorkBuddy Skill ZIP imports with explicit manual activation status and the official ESP-VISION logo.
- Publish `@esp-vision/knowledge-mcp` with a reusable service factory, stdio and HTTP transports, a complete validated knowledge snapshot and source provenance. Keep the official plugin connected to the online service.
- Maintain the Knowledge HTTP runtime and anonymous daily aggregate statistics in the main repository, with Node 24, privacy/reporting regressions and persistent-volume container checks.
- Build Knowledge images from the exact verified npm archive and publish GitLab release descriptors binding npm, snapshot, source and image identities; trigger website deployment after qualification gates.
- Add unified retained-log acceptance for SDK, CLI, plugin, Knowledge npm and website integration, with optional P4 hardware and isolated native-client checks.
- Add a shared GitHub Issues reporting entry and English issue forms for IDE, website, Device Core and firmware problems.

### Changed

- Unify ESP-VISION-owned heap allocations around `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, preserve requested DMA capabilities, and set a 16 KiB internal-memory boundary on PSRAM-enabled P4 boards and the P4 template.
- Qualify ESP32-P4/S3 on pinned ESP-IDF v5.5.6 and retain release/v6.1 for ESP32-S31; align release packaging and bilingual environment guidance with the board-specific matrix.
- Make Device Core 0.5.x an SDK-only package and move the management command to Device CLI, with an explicit migration path from Core 0.4.x and consumers pinned to Core 0.5.1.
- Require a manual action for protected default-branch npm publication. Transfer Knowledge releases through internal GitLab registries rather than the DL firmware channel; publish Knowledge npm from qualified protected firmware tags.

### Fixed

- Remove unsupported `copy_to_fb` arguments from Image construction/conversion and unsupported `transform` arguments from Image copy/drawing, including stubs and documentation. Preserve supported `ImageIO.read(copy_to_fb=...)` behavior.
- Reduce the P4X_EYE SPI display transaction queue to one entry so camera startup and LCD transfers share a bounded DMA footprint.
- Dispatch queued VM RPCs at bounded main-thread Python safe points so live camera/LCD applications can serve capture requests while preserving session authentication and output isolation.
- Confirm detached Python startup at VM entry before releasing the CLI session, preserve acknowledged runs after host disconnect, and retain explicit cancellation behavior. Use passive connections for observation and document that execution metadata and stdout belong to the current session.
- Honor omitted aggregate Python and file-read deadlines while retaining bounded setup/protocol waits, cancellation and cleanup. Make packaged execution-deadline regressions deterministic, including expiry before VM entry.
- Lock POSIX serial adapters before opening them and drain diagnostics before reporting `SERIAL_BUSY`, preventing concurrent CLI processes from stealing device bytes.
- Reset browser-flashed devices through the active stub loader to decode status correctly, while preserving automatic-reset failure diagnostics.
- Load hardware fixtures in RAM within the negotiated REPL input limit; bind checks to the selected board/build identity, preserve allocation/timeout/mode-switch failures, and report unmounted storage without formatting it.
- Retain camera and LCD test-pattern JPEGs in hardware artifacts, verify live display dimensions, and require cross-process screen capture while a tight Python loop is active. Exercise persistent camera/LCD applications across independent CLI sessions with verified cleanup.
- Correct CLI validation results, command-specific flags and detached-output guidance; resolve temporary package artifacts through project-local paths or `TMPDIR`. Guide continuous application verification and consent-based feedback in the plugin Skill.
- Preserve Knowledge API return types, public display aliases and canonical documentation links, including normalized histogram semantics. Generate complete snapshots early with the check-stage interpreter and preserve stub annotations on Python 3.8 documentation builders.
- Validate Knowledge versions and complete MicroPython snapshots in packaged acceptance; separate SQLite initialization contention from concurrent acknowledged-write atomicity using ready writers and an explicit start barrier.
- Require shared host regressions before CLI/plugin publication, verify pinned Core archive integrity, and align acceptance with exact package versions. Correct strict Chinese hardware-guide markup and keep SDK-only fixture regressions independent of a local Python interpreter.

## [2026.10.19]

This release introduces EV-MUX v4, prepares Device Core 0.4.0 browser flashing and release-manifest support, and adds NanoTrack, camera IPA/ISP controls and LCD touch input. Firmware release numbers, npm SDK versions and IDE versions are independent.

### Added

- Prepared Device Core 0.4.0 with immutable publication-manifest readers and verified application extraction from one complete firmware BIN, preserving layout checks, explicit installation, multi-file model groups and legacy feed compatibility. Immutable browser downloads reuse safe cache entries; model sources remain inside normalized commit-pinned paths.
- Added the Device Core `/browser` ROM adapter using pinned tasmota 9.4.2, centralizing P4/S31 browser compatibility and verified install/update workflows, with browser bundle and package isolation checks and bilingual API documentation.
- Added immutable per-version firmware manifests with hash-bound current/history pointers, actual board membership, commit-pinned website/model/MCP snapshots and compiler-derived Wi-Fi capabilities. Compatibility full/app/per-board images remain the default; explicitly enabled single-bin mode extracts qualified application slices from the complete BIN. All compiler contexts for repeated network-service sources must agree on Wi-Fi capability, and upload waits for the complete firmware/HIL/SDK/package qualification contract.
- Added the independently versioned [`@esp-vision/device-core`](https://www.npmjs.com/package/@esp-vision/device-core) host SDK in `host/device-core`. The SDK provides the grouped `createDevice()` API for connections, inspection, Wi-Fi, Python, files, capture, discovery and firmware updates, plus Node TCP/serial adapters, a management CLI and runnable package checks. The package uses Apache-2.0 and preserves historical MIT notices.
- Added EV-MUX v4 session ownership, leases, keepalive and bounded I/O across USB-Serial-JTAG, USB-OTG CDC, UART and TCP. Session and VM generations isolate REPL input/output, logs, preview and capture from stale owners and resets.
- Added background native/Hosted Wi-Fi with persisted provisioning and reconnect, a TCP endpoint and `_esp-vision._tcp.local` discovery. Network/TCP services default to enabled on supported Wi-Fi boards; credentials are still required. Board name, MAC suffix and firmware version form one canonical hostname shared by discovery and interface queries.
- Added reliable binary file upload for Python source, models and images through Core `files.write()`: raw blocks up to 16 KiB, CRC32, sequential offsets, length/SHA-256 verification and explicit commit/abort. The firmware holds one temporary native file open and cleans failed or stale transfers without replacing the existing target before verification.
- Added session-bound storage-writer leases and typed launcher boot/trial status for filesystem handoff and script lifecycle management.
- Added dedicated LCD screenshot capture through Core `capture({ source: 'screen' })`, backed by `debug.capture_screen`, with explicit not-ready/unsupported errors.
- Added `sensor.set_ipa()`, `get_ipa()`, `set_isp()`, `get_isp()` and `get_ipa_stats()` for runtime camera control and statistics, with stubs, documentation and examples. IPA/ISP control is enabled on `ESP32_P4X_EYE` and `ESP32_P4X_FUNCTION_EV_BOARD`; availability depends on the camera and board configuration.
- Added `espdl.NanoTrack`, NanoTrack V2 INT8 models and a BOOT-button-initialized single-object tracking example. The bundled model exports and target restrictions are documented under `models/nanotrack_v2/`.
- Added `lcd_touch.Touch` with `read()` and `deinit()`, with coordinate-reading and drawing examples for boards with configured touch hardware, including the P4 Function EV Board and S31 Mosaico.
- Added application-only firmware images and per-board/version metadata for verified ROM updates through Core: image hashes, partition bounds and chip revision limits. Updates require a compatible existing partition layout.
- Added explicit complete-image installation through Core `firmware.check/update({ mode: 'install' })` and the CLI over ROM-capable USJ/UART ports, without an EV-MUX connection. Release tooling qualifies merged bootloader/partition/application hashes, intersected silicon revision bounds and Flash capacity. Default updates retain their application-only behavior; historical images without qualified metadata cannot be installed through this flow.
- Added bilingual Device Core and EV-MUX documentation covering APIs, architecture, physical interfaces, ownership, Python/files, preview/capture and host integration, plus Core-based hardware-in-the-loop checks on the P4 Function EV Board.

### Changed

- **Breaking host-protocol change:** EV-MUX v3 hosts must move to a matching v4 host/SDK when installing this firmware. Device Core 0.2.0 uploads require firmware capability `file.write.binary`; older v4 firmware remains queryable but must be updated before uploading. Use Core for framed Python/file operations and session coordination.
- Unified host integration around Host applications → Device Core → EV-MUX → firmware. SDK and firmware are maintained together; IDE and Agent applications remain separate consumers. This firmware release does not ship the IDE 1.3.0 application or Agent plugins.
- Gated MSC/VFS handoff across live file operations. FAT file replacement retains an old-file backup for ordinary rename rollback; it is not a power-loss-atomic journal.
- Upgraded ESP-DL from 3.3.6 to 3.3.13, enabled dual-core NanoTrack inference on ESP32-P4 and optimized tracker preprocessing.
- Renamed board default credential macros to `ESP_VISION_WIFI_DEFAULT_SSID` and `ESP_VISION_WIFI_DEFAULT_PASSWORD`; saved NVS profiles and protocol fields retain their meaning.
- Unified CLI queries under `inspect`, with OS-aware USB serial discovery, macOS callout deduplication, Unicode-aware output and `--all-ports` for native UART/Bluetooth candidates. Explicit port connections remain available.
- Unified manual and CI hardware acceptance under `host/device-core` and `npm run test:hardware`. SDK checks/publication are separate from firmware builds; targeted board/IDF verification and the final build matrix remain available.

### Fixed

- Reserved 16 KiB for each EV-MUX transport-task stack to prevent stack-protection resets during nested Wi-Fi configuration parsing and hashing over USB or TCP.
- Retained the project description in firmware CI artifacts so release metadata binds to the actual build version. Release packages validate complete install/update metadata, image hashes, partition bounds and root publication paths before upload; historical releases without qualified metadata remain unavailable for Core flashing.
- Corrected English release-guide title underlines for strict HTML/PDF documentation builds.
- Fixed LCD framebuffer reuse before color DMA completion, balanced P4X-EYE display DMA/JPEG memory use and released resources after failed initialization. Camera and LCD capture use lazy, reusable buffers with lifecycle cleanup.
- Reserved P4X-EYE VFS registration headroom so repeated camera resets can recreate video devices after network and storage initialization.
- Bounded serial startup draining, same-link reacquisition and stale TCP socket reclamation; preserved ownership, partial-frame boundaries and epoch-scoped output across soft resets and backpressure.
- Unified Python and deferred USB DTR/RTS bootloader entry, preserving P4 revision 3.2 system-control bits and USB control completion before restarting.
- Prevented automatic formatting of damaged or unknown Flash FAT, reported storage writer/mount failures and guarded SD synchronization on unsupported boards.
- Aligned ESP-IDF hostnames with the 63-byte DNS limit, reserved DHCP option space and corrected mDNS 1.1 STA-only event/header guards.
- Fixed NanoTrack model compatibility and ESP32-P4 inference crashes; reduced S31 KORVO/CHATBOT camera capture buffers to two to leave more PSRAM for inference and display.
- Selected the supported ESP Video CSI format-conversion path on P4 releases with a backported API, including ESP-IDF 5.5 snapshots.
- Fixed Windows QSTR preprocessing with ESP-IDF 6.1 by evaluating response-file generator expressions and retaining required include paths; corrected CI image/tool availability and hardware-response parsing.
- Fixed standalone SDK exports and installed-package test assets, and avoided false npm publication failures while registry metadata propagates by retrying verification reads without repeating publication; 0.3.0 allows a bounded 151-second propagation backoff.
- Fixed README logos/architecture diagrams across light/dark GitHub and GitLab renderers, CLI column alignment, bilingual RST formatting and MCP/documentation references.

### Removed

- Removed duplicate Python EV-MUX clients, obsolete v3 stress/runtime tools and separate Agent smoke/recovery runners with their implementation-specific tests. Core hardware checks and native firmware contract/fault tests provide the maintained validation entry points.

## [2026.08.27]

### Added

- Added the shared ESP-DL PicoDet pedestrian model, an `espdl.PedestrianDetect` wrapper with model-specific preprocessing and post-processing, Python stubs, and a camera example for ESP32-P4/S31/S3 boards.
- Added a button-triggered PP-OCRv6 pipeline with separate INT8 text detection and recognition stages, Python detection/CTC post-processing, and generic BGR preprocessing through `espdl.Model`.
- Added ESP32-S3 USB-OTG CDC automatic download-mode entry using the esptool USB-Serial/JTAG DTR/RTS sequence, controlled per board from `mpconfigboard.h` and handled outside the MicroPython VM. The existing EV-MUX CDC stack now starts before camera initialization, filesystem recovery, and `boot.py`; its transport task keeps the download path responsive while the VM is busy without introducing a second USB driver or descriptor owner.
- Added an offline imlib allocator regression covering fallback allocation, nested marks, rollback, and failure cleanup.
- Added OpenMV v5.0.0's MIT-licensed Edge Drawing Lines implementation and enabled `Image.find_line_segments()` on all supported boards.
- Added initial `ESP32_S31_CHATBOT` support (requires ESP-IDF release/v6.1) with a USB Serial/JTAG console, an SC101IOT DVP camera, and a 480x480 CO5300 QSPI display managed by ESP Board Manager.
- Added initial `ESP32_S31_MOSAICO` support on ESP-IDF release/v6.1 with native USB 2.0 High Speed CDC/Flash MSC, an OV3640 DVP camera accepting both official product IDs (`0x364C` and `0x3641`), and a 480x480 CO5300 QSPI display. Cold-start eFuse detection treats the legacy all-zero value as v1.0 and selects the v1.0 or shared v1.1/v1.2 LCD pin map while retaining the CameraBoard's common wiring; camera startup enables the shared GPIO60 VCC_3V3 rail, releases the built-in USB Serial/JTAG pads shared by Camera D2, and waits for the external oscillator before SCCB probing. The generated `main.py` previews camera frames on the LCD with a default 90-degree counter-clockwise PPA rotation. The board uses onboard NAND Flash instead of an SD card; NAND storage is not yet exposed and camera flash support remains disabled.
- Added `ESP32_S31_MOSAICO` to the release manifest so tagged releases publish its merged firmware in the website's ESP32-S31 download and online-flashing selector.
- Added a camera-independent LCD color-bar and solid-color diagnostic under `example/06-Peripherals/01-Display`.

### Changed

- Updated the `ESP32_S31_MOSAICO` generated `main.py` to capture VGA, crop the center 480x480, and fit that square to the 480x480 display.
- Updated the ESP Video dependency to version 2.4.0 across the supported ESP-IDF release overlays.
- Added periodic MicroPython event polling to long-running imlib drawing, filtering, feature-detection, QR, statistics, and template-matching loops.
- Migrated supported boards' camera and LCD initialization to ESP Board Manager, including runtime OV3660 and SC101IOT format selection and sensor-specific orientation correction on `ESP32_S31_KORVO`; the `TEMPLATE` board remains an explicitly documented non-Board-Manager starting point.
- Synchronized the MicroPython Wi-Fi authentication constants with the native and remote Wi-Fi backends in ESP-IDF 6.0 and 6.1.
- Updated `esp_wifi_remote` to 1.6.4 for every supported ESP-IDF release overlay.
- Moved Flash and SD FAT filesystem ownership to an ESP-IDF storage manager, with native MicroPython VFS bridges at `/` and `/sdcard` and a shared raw Flash backend for the existing MSC LUN.
- Exposed SD cards as MSC LUN 1 on SD-capable boards while retaining Flash as LUN 0.
- Moved the ESP-VISION USB-Serial-JTAG interface selection to each board's `mpconfigboard.h`, explicitly enabled the USJ clock and internal PHY from the MicroPython driver, returned the ESP-IDF system console to its target defaults, removed redundant board sdkconfig overrides, and clarified transport reliability and observability documentation.
- Changed the ESP32-S3 automatic-download hand-off to defer the DTR/RTS-triggered reset out of the TinyUSB line-state callback and independently apply the MicroPython-proven USB persistence sequence before entering ROM USB-Serial/JTAG download mode.
- Documented esptool `--after watchdog-reset` for leaving an ESP32-S3 ROM loader entered through USB-Serial/JTAG.

### Fixed

- Delayed the EV-MUX transport task until TinyUSB initialization completes, guaranteed at least one RTOS tick between pumps, and configured the ESP32-S3 boards for a 1000 Hz FreeRTOS tick, preventing USB startup races and MicroPython task starvation.
- Fixed EV-MUX script execution to close the source file before running it, allowing a script to be overwritten immediately after it finishes.
- Fixed ESP32-P4X-EYE LCD updates under camera and inference memory pressure by sending the PSRAM framebuffer through SPI DMA directly.
- Fixed EV-MUX CDC startup on boards with a 100 Hz FreeRTOS tick by guaranteeing the higher-priority transport task delays for at least one tick instead of starving the MicroPython main task with `vTaskDelay(0)`; CDC hello discovery now waits for DTR-ready state, and USB serial descriptors encode only the initialized six-byte factory MAC.
- Fixed the EV-MUX stress test to expect non-active-sink discovery responses on their request ingress, matching the protocol v3 RPC response contract.
- Fixed EV-MUX path validation so script/filesystem paths are limited to 123 bytes, leaving room for the transactional `.tmp` suffix within MicroPython's 128-byte VFS path capacity; over-limit paths return `PATH_TOO_LONG` without entering VFS or resetting the VM.
- Fixed Wi-Fi authentication constant exposure after the remote Wi-Fi header update by removing the deprecated WPA3 extension bindings that no longer exist in the active API; the IDF 6 unknown-scan mode remains exposed where that enum is provided.
- Fixed IDF 5.5 builds on AtomS3R-M12 by accepting both Wi-Fi authentication enum layouts used by the release and stable 5.5 headers without changing managed components.
- Fixed internal-preferred fast-framebuffer allocations incorrectly reporting exhaustion when the internal largest block was small but a usable PSRAM block remained; AprilTag, QR, display, and heap-backed UMM temporary allocations now unwind through balanced normal and exceptional paths.

### Removed

- Removed generic `vfs.VfsFat` support from ESP32 firmware now that board Flash and SD FAT volumes are owned by ESP-IDF; `vfs.VfsLfs2` remains available for independent block devices.

## [2026.07.28]

### Added

- Added Windows host support for repository-root `idf.py --board <BOARD> ...` firmware builds.
- Added an agent-oriented MCP knowledge guide that requires ESP-VISION code to target the supported MicroPython runtime, defaults generated applications to an all-in-one script, rejects unprovisioned `requirements.txt` dependencies, and provides a copyable USB-OTG CDC/USJ EV-MUX closed-loop test.

### Fixed

- Fixed tag release packaging after the application binary was renamed from `micropython.bin` to `esp-vision.bin`, updated the launchpad merge step to validate its input artifacts and use the current esptool command syntax, and deferred GitHub synchronization until the deploy stage succeeds.
- Silenced known deprecation and shadow warnings from the current `esp_video` and `esp-tflite-micro` managed component releases without disabling those warnings for ESP-VISION sources.
- Fixed clean firmware builds by providing the Git-derived firmware version to MicroPython's qstr preprocessing pass.
- Fixed ESP-IDF 6.1 firmware builds by synchronizing MicroPython's Wi-Fi authentication constants with the new unknown scan-result mode.
- EV-MUX protocol v3 with two routed streams: `user` (`user.rpc`, framed REPL, and `preview.frame`) and `debug` (`debug.rpc` and `log.idf`). The transport task parses independent USJ, CDC, and UART ingress streams while the VM is busy.
- EV-ATP host operations for discovery, stream routing, script upload/run, device control, transport statistics, filesystem/sensor inspection, and binary JPEG capture through `debug.rpc`.
- DTR-driven routing: both streams follow USB-OTG CDC while its port is open and fall back to the board's default sink when it closes; `route.bind` and `route.auto` support per-stream overrides.
- Preview flow control drops whole `preview.frame` frames from congested sinks and caps the USJ preview rate to one frame per 100 ms. `debug.info` scope `transport.stats` reports RX, parser, queue, timeout, and drop counters.
- `capabilities` reports the board USB product string and distinguishes CDC `present` (enumerated) from `ready` (DTR asserted).
- `hello`, `capabilities`, and device diagnostics expose a stable `firmware.id` plus the Git-derived ESP-IDF `PROJECT_VER`, allowing IDEs to distinguish release and development firmware builds without coupling protocol compatibility to a release string.
- VM-context RPCs queued for more than 5 seconds now fail with `VM_TIMEOUT` instead of occupying the queue indefinitely.
- EV-MUX replies return on the request ingress, including discovery requests received on a non-active USB sink.
- Single-CDC boards now follow CDC DTR instead of remaining on an unavailable USJ route.

### Changed

- `img.flush()` now sends binary JPEG data in `preview.frame` instead of the previous base64 `EVFRAME` text envelope.
- Python stdout/stderr, C stdio, ESP-IDF logs, and debug output are framed on their assigned EV-MUX channels while mux mode is enabled.
- The REPL banner and help text, runtime metadata, ESP-IDF application name, USB descriptors, and recovery/crash messages now use ESP-VISION branding.
- Architecture documentation now describes the two-stream routing, framing, RPC, and execution model.
- The top-level Make build interface; firmware builds now use the repository-root `idf.py --board <BOARD> ...` CMake interface exclusively.

## [2026.07.16]

### Added

- Added ESP-IDF release/v6.1 support for all supported boards, including `ESP32_S31_KORVO`.
- Replaced the root `CLAUDE.md` with `AGENTS.md` to provide shared project guidance for AI coding agents.
- Added quick-access links to the ESP-VISION website and Web IDE in the English and Chinese introduction pages, including guidance to the website's MCP setup resources.
- Added a low-brightness status blink on the `ESP32_P4X_VISION` board's GPIO9 WS2812 in the default first-boot `main.py`, with the NeoPixel driver frozen into that board's firmware.
- Added `ESP32_P4X_VISION` to the esp-launchpad release manifest and packaging job so release tags publish flashable firmware for the board.

### Fixed

- Fixed sequential LCD scripts by transferring teardown ownership of the shared board display to the newest successfully initialized `display.Display` object, so delayed finalization of an older wrapper cannot deinitialize the LCD reused by the current script.
- Fixed the board default LCD preview scripts and the `lcd_preview.py` example to release `sensor` and `display` resources on `Ctrl-C`, preventing retained LCD buffers from reducing the fast framebuffer memory available to later AprilTag runs.

### Removed

- Removed the ESP-IDF master component manifest and build fallback; firmware builds now support only release/v5.5, release/v6.0, and release/v6.1.

## [2026.06.27]

### Added

- Added `sensor.VGA` framesize support for camera output on supported P4 and S31 boards.
- Added an ESPDet Pico hardhat model and English/Chinese documentation for training ESPDet Pico models.
- Added a generic `espdl.Model` runner that exposes ESP-DL input/output tensor metadata and raw output bytes for Python-side post-processing, plus `example/03-Machine-Learning/00-ESP-DL/espdet_pico_python.py` as an ESPDet Pico reference using Python decode and NMS.

### Fixed

- ESP32_S31_KORVO camera startup now drives XCLK and SCCB I2C from board code before `esp_video` initialization, applies an OV3660 soft reset, and retries the DVP stream/init path internally before Python sees an error.
- MCP API knowledge pack generation for the documentation sidecar, published at release time under `mcp/latest.json` and `mcp/mcp-pack-<tag>.json`, with CI schema validation.
- `config.toml` `[[website.models]]` entries now include `downloadUrl`, derived from each model's repository path and pointing to the public GitHub `master` raw URL for website model downloads.

### Fixed

- MCP pack MicroPython extraction now excludes ESP8266-only `esp` APIs from the ESP32 target set.
- MCP pack RST parsing now ignores Sphinx directive options and joins wrapped signatures, avoiding invalid entries such as `:noindex:` or orphaned continuation lines.
- Release publishing now copies `mcp/latest.json` only after pack generation and validation both succeed.

## [2026.06.22]

### Added

- TensorFlow Lite Micro support via a new `tflite` module (`tflite.Model`) on ESP32-P4/S3/S31: run `.tflite` models with ulab ndarray input/output, exposed quantization metadata, and an optional post-processing callback. Ships two bundled models under `models/tflite/` (`person_detect`, `sine`) and examples under `example/03-Machine-Learning/01-TFLite/` (`person_detection.py`, `sine.py`).
- New board `ESP32_P4X_VISION` (ESP32-P4, 16 MiB flash layout): SC101IOT DVP camera backend, SDMMC slot 0, and flash MSC. Display is a board-level placeholder until the LCD hardware is enabled.
- New example `example/.../01-Cloud-AI/openai_compatible_vision.py`: send camera frames to an OpenAI-compatible vision API from the board.
- esp-launchpad `config.toml` now lists the bundled AI models under `[[website.models]]`. Each model lives in any `models/` subfolder as a binary plus a same-named `.json` sidecar (`name`, `architecture`, runtime `api`, `task`, `input`, `inputFormat`, `dataset`, `labels`, `sizeBytes`, and optional `description`/`datasetUrl`); the generator scans all subfolders for sidecars, so the website can list what ships with a release. Adding a model is just dropping in both files; the size is cross-checked against the binary.
- `config.toml`'s `[website]` table now carries `releases` (every published firmware tag, newest first) and `binaryNameTemplate` (`esp-vision-{board}-{tag}.bin`). Because the naming is consistent, the website builds any historical version's firmware download URL itself from `firmware_images_url` + the template, without listing each version's bins.

### Changed

- `config.toml` `[website].schemaVersion` is now a semver string (was the integer `1`); consumers pin on the major component.

## [2026.06.17]

Initial release.
