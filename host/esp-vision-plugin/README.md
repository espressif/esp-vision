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

## Device CLI and Knowledge MCP

The plugin includes a dependency-closed `cli/device-cli.mjs` bundle with its Python serial and ROM adapters and original dependency notices. The Skill runs that installed copy by absolute path and selects commands for the user's request. It does not require the user to install the CLI package globally or through `npx`. The separate `@esp-vision/device-cli` npm package remains available for people who want the shell tool outside Codex or Claude Code.

The Knowledge MCP is hosted at `https://mcp.vision.espressif.com/mcp`; firmware documentation is not copied into the package. The plugin's only MCP connection is that online Knowledge MCP. The Agent retains its own AI loop and uses a local shell executor for device operations.

After a CLI operation, inspect the result before retrying if it timed out, was cancelled, or returned an uncertain outcome. CLI commands release their connection when they finish. If another client owns the selected device, release that session in its owner first; do not force takeover. Cancellation does not roll back device changes or prove that a started Python program stopped.

## Host adapter contracts

| Host | Manifest and catalog | Knowledge MCP |
| --- | --- | --- |
| Codex | Portable `plugin.json`; `.agents/plugins/marketplace.json` | `mcp.json`, remote `streamable-http` |
| Claude Code | `.claude-plugin/plugin.json`; `.claude-plugin/marketplace.json` | `.mcp.json`, remote `http` |
| WorkBuddy | Local Skill ZIP uploaded through WorkBuddy UI | Separate `mcp.json`, remote `streamableHttp`; WorkBuddy UI activation required |

Codex and Claude discover the same `skills/esp-vision-development/SKILL.md`. WorkBuddy receives a format-adapted copy in the ZIP, with its documented `@references/` syntax and a persistent absolute path to the installed `cli/device-cli.mjs`. Its ZIP also carries the full dependency-closed CLI, official logo and provenance, and license notices. All hosts use the same installed CLI artifact. Packaging checks manifest/version integrity, absence of an embedded device MCP, and isolated CLI `--help` and `inspect` runs using a fake Python serial runner, including the copy extracted from the WorkBuddy ZIP. This does not substitute for app activation and acceptance; WorkBuddy's Skill import and Knowledge MCP connection require the documented UI steps.

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
