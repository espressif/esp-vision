# ESP-Vision AI视觉

Author: Espressif Systems (Shanghai) CO LTD. Contact: libo@espressif.com. Expert version: {{VERSION}}. Source plugin/npm version: {{SOURCE_VERSION}}.

ESP-Vision helps develop ESP32-based vision applications. This Expert ZIP bundles an Agent, the ESP-Vision development Skill, the original dependency-closed device CLI, required third-party license notices, and the official Knowledge MCP dependency. It is a new host-format export, not an npm release or proof of WorkBuddy activation.

## Runtime requirements

Use the local WorkBuddy desktop execution environment with Node.js 22 or newer. Serial access requires Python with `pyserial`. Firmware flashing additionally requires `esptool>=5.3,<6`. Install missing runtimes only with the user's authorization and chosen environment. The package does not automatically install runtimes. Documentation needs network access to `https://mcp.vision.espressif.com/mcp` and no device connection. Device operations use the bundled CLI and a user-selected serial or TCP endpoint.

## Publication and activation

Upload the ZIP through the [WorkBuddy Open Platform](https://open.workbuddy.cn/) after developer/enterprise registration and certification. Create the Expert business entry, upload the archive, inspect parsed metadata, test the saved expert, then submit for review. Approval and publication are separate steps. See the [platform workflow](https://open.workbuddy.cn/docs) and [Expert specification](https://open.workbuddy.cn/docs/expert). No upload, certification, review approval or publication is implied by this package.

On first summon, complete WorkBuddy's Knowledge MCP dependency connection guide. Confirm the expert and Skill activate, the MCP can answer a documentation question, and the installed bundle retains the CLI. Use the root substituted by the host in `node "${CODEBUDDY_PLUGIN_ROOT}/cli/device-cli.mjs" --help`. If it is not substituted, resolve the actual installed plugin path as described in the Skill; do not guess a project-relative path. Host path substitution, Windows/macOS activation and real hardware operations require target-host acceptance.

The published npm 0.1.0 WorkBuddy installer still creates its original separate Skill ZIP and MCP export. This archive is the Expert publication artifact; preparing it does not change that immutable release.

## Maintainer acceptance

Verify ZIP parsing and all bilingual listing fields, the avatar's display and marketplace style acceptance, Agent/Skill loading, Knowledge connection, retained CLI resources, paths containing spaces, and target operating systems. After selecting a device, separately verify real enumeration, requested Python execution/output and captures. A mock serial smoke test does not establish hardware support.
