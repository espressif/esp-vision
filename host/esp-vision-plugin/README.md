# ESP-VISION plugin

The plugin combines the standalone ESP-VISION Device CLI, the official online Knowledge MCP, and one capability-oriented Skill. The Skill chooses the local CLI or documentation tools according to the task. It invokes the CLI copy stored inside the installed plugin; no device MCP server is bundled or registered.

## Requirements

Use Codex, Claude Code, or WorkBuddy, Node.js 22 or newer on PATH, a local shell executor, and network access to `https://mcp.vision.espressif.com/mcp`. Serial operations also require Python 3 with `pyserial` and access to the selected port. Firmware flashing requires Python `esptool>=5.3,<6`. TCP device operations do not require serial Python support. Commands run on the computer that can reach the board; cloud-only execution cannot access that computer's USB port.

## Install from npm

Choose the client installed on this computer and run one command using a published package version:

```sh
npx -y @esp-vision/plugin install codex
# Or:
npx -y @esp-vision/plugin install claude
# Or:
npx -y @esp-vision/plugin install workbuddy
```

The installer validates the bundled CLI, Skill, manifests, and notices, then copies them into a stable versioned user data directory. It registers the local marketplace and installs `esp-vision@esp-vision` with the selected client's native CLI. The plugin remains available after npm clears its download cache. Start a new client session to load its Knowledge MCP and Skill.

To preview a first installation without writing files or invoking the client:

```sh
npx -y @esp-vision/plugin install codex --dry-run
```

For WorkBuddy, the installer copies the same verified runtime to the stable user data directory and prepares a WorkBuddy Skill ZIP, Knowledge MCP JSON, and setup guide under `workbuddy/<version>/`. It does not modify WorkBuddy configuration or claim that the app has activated the integration. Follow `INSTALL.md`: upload `esp-vision-development.zip` in **专家 · 技能 · 连接器 > 技能 > 添加技能 > 上传技能** (Expert · Skill · Connector > Skills > Add Skill > Upload Skill), then paste `mcp.json` under **插件 > MCP 服务器 > 配置 MCP** (Plugins > MCP Server > Configure MCP). Confirm the MCP connection and start a new conversation. The installer reports `manual-install-required` until those WorkBuddy UI steps are completed.

Use `--data-dir /absolute/path` to select another persistent data directory. To pin an installation, use `npx -y @esp-vision/plugin@0.1.0 install codex`. Repeating the same version verifies the native registration and reuses the existing files. When upgrading, the installer validates the old marketplace path and version from the native client lists and verifies the previous installer-owned bundle before changing registrations. It copies the new version first, removes and re-registers only the ESP-VISION plugin and marketplace through the native client, and leaves prior versioned files in place. Claude upgrades retain the prior installation scope and use `--keep-data`. If a native command fails, the installer reports the observed marketplace and plugin state plus a rerun path; resolve that client error and rerun the installer. The installer does not edit client configuration files directly.

### Installation identity and recovery

A version string alone does not establish that an installed directory matches the official immutable npm archive. An earlier development copy can have the same version but different manifests or missing resources; the installer rejects that mismatch rather than overwriting it. Preserve the full installer-owned runtime directory and native plugin cache before recovery, including any local changes. Use the host's native commands to uninstall the ESP-VISION plugin and remove its marketplace registration, retaining user data where supported. Confirm ownership and move the backed-up old installer-owned `plugins/<version>` directory aside, then reinstall the pinned official release and restart the host. Verify the native registrations and installed resources. Do not hand-copy missing assets, force an overwrite or discard an unrecognized directory. `@esp-vision/plugin@0.1.0` does not provide a `--repair` option.

WorkBuddy's `manual-install-required` result confirms export preparation, not native installation or activation. The legacy installer's contract remains the Skill ZIP plus separate Knowledge MCP configuration and a new conversation after both UI steps. Maintainers can also prepare the separate Expert ZIP described below; it does not change the npm install command or prove host activation.

## Device CLI and Knowledge MCP

The plugin includes a dependency-closed `cli/device-cli.mjs` bundle with its Python serial and ROM adapters and original dependency notices. The Skill runs that installed copy by absolute path and selects commands for the user's request. It does not require the user to install the CLI package globally or through `npx`. The separate `@esp-vision/device-cli` npm package remains available for people who want the shell tool outside Codex or Claude Code.

The Knowledge MCP is hosted at `https://mcp.vision.espressif.com/mcp`; firmware documentation is not copied into the package. The plugin's only MCP connection is that online Knowledge MCP. Its tools provide read-only documentation lookup and do not submit issues or anonymous feedback. Report functional problems through [GitHub Issues](https://github.com/espressif/esp-vision/issues); prepare a template-based English draft and obtain user authorization before submitting it or its attachments. Follow [SECURITY.md](https://github.com/espressif/esp-vision/security/policy) for private vulnerability reporting. The Agent retains its own AI loop and uses a local shell executor for device operations.

This split is deliberate: the absence of Device MCP tools alone is not a defect and does not require another MCP wrapper. Before using hardware-specific APIs or live capture, check official knowledge against the selected board and actual firmware. Driver ISP readback is not a per-frame synchronization fence, IPA state does not establish the sensor's AE/AWB state, and advertised capture support on older firmware does not by itself guarantee capture during a running application. The packaged application workflow reference explains these boundaries and how to retain evidence of real failures.

After a CLI operation, inspect the result before retrying if it timed out, was cancelled, or returned an uncertain outcome. CLI commands release their connection when they finish. If another client owns the selected device, release that session in its owner first; do not force takeover. Cancellation does not roll back device changes or prove that a started Python program stopped.

## Host adapter contracts

| Host | Manifest and catalog | Knowledge MCP |
| --- | --- | --- |
| Codex | Portable `plugin.json`; `.agents/plugins/marketplace.json` | `mcp.json`, remote `streamable-http` |
| Claude Code | `.claude-plugin/plugin.json`; `.claude-plugin/marketplace.json` | `.mcp.json`, remote `http` |
| WorkBuddy | Local Skill ZIP uploaded through WorkBuddy UI | Separate `mcp.json`, remote `streamableHttp`; WorkBuddy UI activation required |
| WorkBuddy Expert export | `.codebuddy-plugin/plugin.json`, Agent and Skill in one ZIP | Declared `.mcp.json` dependency, remote `http`; platform parsing and activation require target-host acceptance |

Codex and Claude discover the same `skills/esp-vision-development/SKILL.md`. The legacy WorkBuddy adapter receives a format-adapted copy in the ZIP, with its documented `@references/` syntax and a persistent absolute path to the installed `cli/device-cli.mjs`. Its ZIP also carries the full dependency-closed CLI, official logo and provenance, and license notices. All hosts use the same installed CLI artifact. Packaging checks manifest/version integrity, absence of an embedded device MCP, and isolated CLI `--help` and `inspect` runs using a fake Python serial runner, including the copy extracted from the WorkBuddy ZIP. This does not substitute for app activation and acceptance; WorkBuddy's Skill import and Knowledge MCP connection require the documented UI steps.

## Build and package

Build from the repository root after installing the Device CLI package dependencies:

```sh
npm --prefix host/device-cli install
npm --prefix host/esp-vision-plugin run build
npm --prefix host/esp-vision-plugin test
```

Create a release archive and SHA-256 sidecar with:

```sh
npm --prefix host/esp-vision-plugin run archive -- --output-dir "$PWD/.cache/plugin-release"
```

The archive command extracts and checks the produced archive, including the isolated CLI smoke. The main repository release workflow validates the npm tarball and registry integrity before publication. Publication uses protected main-repository CI and requires the protected `NPM_PUBLISH_TOKEN` credential. Bump the plugin version and keep both native plugin manifests aligned before a release; an existing npm version is accepted only when its registry integrity equals the generated tarball.

### WorkBuddy Expert ZIP

After building the dependency-closed plugin with `npm run build`, create the separate Expert publication artifact:

```sh
npm --prefix host/esp-vision-plugin run build:workbuddy-expert -- --output-dir "$PWD/.cache/workbuddy-expert"
```

The Node.js builder reuses the existing ZIP encoder and the validated plugin's CLI, Python helpers and license texts. It writes `esp-vision-workbuddy-expert-<version>.zip`, a SHA-256 sidecar and a receipt listing file digests, without overwriting existing artifacts. Output must be outside the plugin source. To export an already built, verified plugin from another directory, pass `--source /absolute/prebuilt/plugin`; templates still come from this checkout. No Python build script or image conversion tool is required. The committed 512×512 avatar retains the official vector camera symbol; its source and provenance accompany the export.

The ZIP contains bilingual Expert metadata with the original publisher identity **Espressif Systems (Shanghai) CO LTD**, contact `libo@espressif.com`, an Agent, preloaded Skill and its application reference, and the official Knowledge MCP dependency. Instructions use `${CODEBUDDY_PLUGIN_ROOT}` and a checked installed-Skill fallback instead of the export machine's absolute paths. No Device MCP is added. Node.js 22+, Python/pyserial for serial access, and esptool for flashing remain runtime requirements.

#### Maintainer constraints

- Expert `displayName` values must be nonblank and at most **15 characters per locale**, including spaces, ASCII letters and Chinese characters. Use `ESP-Vision AI视觉` (15 characters) in Chinese and `ESP-Vision AI` (13 characters) in English; keep the manifest, Agent and Skill names consistent.
- Expert versions must be at most **20 characters**. Candidate versions use the pipeline ID; retain the full source SHA in `BUILD-PROVENANCE.json` and the receipt instead of extending the version.
- Keep the Chinese listing description at 40–50 characters and explicitly describe the ESP32-based scope; include `ESP32` in keywords and visible bilingual tags.
- Enforce these constraints in packaging and release validation, test accepted/rejected boundaries, and inspect the actual final ZIP's metadata before delivery. Any new platform parser constraint reported by the user must be recorded in this component guide and added to validation before another ZIP is supplied. Do not claim WorkBuddy parsing or activation without platform evidence.

This is a candidate for the [WorkBuddy Expert upload workflow](https://open.workbuddy.cn/docs/expert). The local tests validate ZIP integrity, portable metadata, retained CLI/licenses and mock serial execution from a path containing spaces. They do not establish platform parsing, publisher eligibility, avatar approval, WorkBuddy Agent/Skill activation, root substitution/resource retention, Knowledge connection, Windows/macOS behavior or real-device operation. Verify those in the target portal and desktop before publication. The builder performs no upload, host installation or device operation; it neither publishes nor replaces npm `0.1.0`.

### Official specification review (2026-10-27)

Checked the live [Expert specification](https://open.workbuddy.cn/docs/expert), its downloadable template and the [Skill specification](https://open.workbuddy.cn/docs/skill) against the actual CI archive, not just source templates. Directory references, required bilingual metadata, Agent identity/profession, Skill fields, three tags/prompts, first-prompt equality, MCP dependency/icon resolution and the PNG's dimensions/size match the documented structure. The archive tests exercise these fields and referenced files together.

The linked example ZIP conflicts with the current page: it declares Agent `tools`, uses an older category and references a missing avatar. Follow the current written specification: omit Agent `tools`, use `02-Engineering` and retain the referenced avatar. Do not copy the sample wholesale. The 15-character name and 20-character version limits remain additional constraints observed from actual platform rejections; the public field table does not state them.

The official camera-symbol avatar satisfies technical image checks; its acceptance under the platform's illustration-style requirement is pending marketplace review. The page does not specify `${CODEBUDDY_PLUGIN_ROOT}` substitution or guarantee retention of arbitrary CLI resources. Keep the documented installed-Skill fallback and verify both behaviors in WorkBuddy. ZIP validation cannot establish parsing, activation or listing approval.

### Internal testing before a release

Every successful MR/branch `plugin_checks` job exports a **WorkBuddy Expert candidate** artifact link. Open that job's artifacts browser and download the ZIP, `.sha256` and `.receipt.json` from `.cache/workbuddy-expert/`; they expire after one month. This CI export does not upload anything to WorkBuddy or a registry. To reproduce CI identity locally, add `--ci` to the builder with `CI_COMMIT_SHA` set to the full source SHA and `CI_PIPELINE_ID` set to a positive test identifier.

Candidate manifests use a unique Expert version such as `0.1.0-dev.1730541`, using the GitLab pipeline ID to distinguish builds within this project; the plugin/npm source version remains `0.1.0`. All Expert versions must fit WorkBuddy's 20-character limit. The builder checks candidate, release and local versions before writing artifacts, and the release publisher checks the limit again before network writes; oversized versions fail explicitly instead of being truncated. The receipt and embedded `BUILD-PROVENANCE.json` retain the full source SHA, source version, Expert version and candidate pipeline. Use those identities and the checksum when reporting results; a shared npm version is not proof that two candidates contain the same code. Do not overwrite an installed same-version development directory or reuse a stale host cache. Keep candidates separate, preserve any local changes, use the host's supported removal/replacement process when necessary, and confirm the version and retained resources after restarting the host.

For local acceptance, inspect the archive and run its bundled CLI `--help` after extraction. For host acceptance, use an authorized unpublished Expert draft in the [WorkBuddy Open Platform](https://open.workbuddy.cn/) and its available configuration/debugging flow. The [Expert specification](https://open.workbuddy.cn/docs/expert) documents the package format; it does not establish a desktop local-ZIP import UI. Verify the actual portal/account and desktop testing entry before proceeding. Uploading a draft is separate from submission, approval and marketplace publication. Test Agent/Skill loading, Knowledge connection, root substitution, retained CLI files, a path containing spaces and the target OS; perform real-device checks only against an explicitly selected endpoint. Record untested items instead of claiming activation.

### Release attachments

On a protected firmware release tag, the same `plugin_checks` builder uses the normalized tag as its Expert version (`2026.10.27` → `2026.10.27`, `v1.2` → `1.2.0`) while preserving the independent npm source version. Release bytes embed the full source SHA and original tag, without a pipeline number, so retrying the same tag/source produces the same artifact. Expert versions follow repository releases instead of reusing the npm version as a host-cache identity. Normalized tag aliases such as `v1.2` and `v1.2.0` denote the same Expert version and must not identify different payloads.

After `qualify_firmware_release`, `upload-dl-trigger` and the release contract checks pass, `publish_workbuddy_expert` consumes the checked ZIP without rebuilding it. It first reads the existing Git tag, Release and asset links to reject source/link conflicts, then uploads and reads back the ZIP/checksum/receipt under Generic Package Registry package `esp-vision-workbuddy-expert`, keyed by the validated Expert version. Tag aliases cannot store different content under the same Expert installation version. Only after the package cohort succeeds does it create a missing Release for the existing original tag and attach all three canonical package URLs. It never creates a Git tag; existing release notes and other attachments are retained. Identical retries are accepted, conflicting bytes or links fail, and no mutable `latest` pointer is written. These links persist beyond CI artifact expiry and retain the GitLab project's access requirements. Project Package Registry/Release permissions and the actual first tag pipeline still require deployment verification.

Expert publication follows the existing firmware release gate but does not add files to the DL firmware bundle, invoke npm publication, replace immutable npm `0.1.0`, or publish to the WorkBuddy marketplace. The separate manual npm publication jobs retain their existing immutable-package checks.
