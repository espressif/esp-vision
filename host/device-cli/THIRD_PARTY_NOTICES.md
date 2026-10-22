# Third-party notices

The CLI bundle includes the following third-party packages. Their original license files are in `licenses/`.

| Package | Version | License | License file |
| --- | --- | --- | --- |
| @leichtgewicht/ip-codec | 2.0.5 | MIT | `leichtgewicht__ip-codec-LICENSE` |
| ansi-regex | 6.4.0 | MIT | `ansi-regex-license` |
| dns-packet | 5.6.1 | MIT | `dns-packet-LICENSE` |
| emoji-regex | 10.6.0 | MIT | `emoji-regex-LICENSE-MIT.txt` |
| get-east-asian-width | 1.7.0 | MIT | `get-east-asian-width-license` |
| multicast-dns | 7.2.5 | MIT | `multicast-dns-LICENSE` |
| smol-toml | 1.9.1 | BSD-3-Clause | `smol-toml-LICENSE` |
| string-width | 7.2.0 | MIT | `string-width-license` |
| strip-ansi | 7.2.0 | MIT | `strip-ansi-license` |
| thunky | 1.1.0 | MIT | `thunky-LICENSE` |

## Preserved source license comments

Original copyright and SPDX comments from bundled source files are reproduced below. Recognized legal comments also remain inline in `dist/device-cli.mjs`.

### Source notice 1

Bundled from: `node_modules/@esp-vision/device-core/src/evrepl.js`

```text
/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 *
 * Framed friendly-REPL client over EV-MUX repl channels.
 *
 * With boot-default EV-MUX firmware there is no raw REPL bootstrap: the
 * device speaks frames from power-on, and the friendly REPL is reachable
 * through repl.stdin / repl.stdout frames. This module executes Python
 * snippets over that channel and resolves their stdout, so device info,
 * the file tree and all filesystem operations (read/write/create/delete)
 * run framed, with no raw REPL access anywhere.
 */
```

### Source notice 2

Bundled from: `node_modules/@esp-vision/device-core/src/device_session.js`

```text
/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 *
 * UI-independent EV-MUX device interpreter. Both the browser IDE and agent
 * integrations construct this class with the same EvMuxParser/EvRepl
 * implementations, so protocol negotiation and firmware operations cannot
 * drift between the two entry points.
 */
```

### Source notice 3

Bundled from: `node_modules/@esp-vision/device-core/src/evmux.js`

```text
/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
```

### Source notice 4

Bundled from: `node_modules/@esp-vision/device-core/src/device.js`, `node_modules/@esp-vision/device-core/src/device_operations.js`, `node_modules/@esp-vision/device-core/src/firmware_updates.js`

```text
/* SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */
```

### Source notice 5

Bundled from: `node_modules/smol-toml/dist/date.js`, `node_modules/smol-toml/dist/error.js`, `node_modules/smol-toml/dist/extract.js`, `node_modules/smol-toml/dist/index.js`, `node_modules/smol-toml/dist/parse.js`, `node_modules/smol-toml/dist/primitive.js`, `node_modules/smol-toml/dist/stringify.js`, `node_modules/smol-toml/dist/struct.js`, `node_modules/smol-toml/dist/util.js`

```text
/*!
 * Copyright (c) Squirrel Chat et al., All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
```

### Source notice 6

Bundled from: `node_modules/@esp-vision/device-core/src/firmware_feed.js`, `node_modules/@esp-vision/device-core/src/firmware_flash.js`, `node_modules/@esp-vision/device-core/src/index.js`, `node_modules/@esp-vision/device-core/src/network_discovery.js`, `node_modules/@esp-vision/device-core/src/network_discovery_node.js`, `node_modules/@esp-vision/device-core/src/node_inventory.js`, `node_modules/@esp-vision/device-core/src/node_rom.js`, `node_modules/@esp-vision/device-core/src/node_transport.js`, `src/cli.js`

```text
// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
```

### Source notice 7

Bundled from: `node_modules/@esp-vision/device-core/src/firmware_io.js`, `node_modules/@esp-vision/device-core/src/firmware_release.js`, `node_modules/@esp-vision/device-core/src/node.js`

```text
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
```

### Source notice 8

Bundled from: `node_modules/@esp-vision/device-core/src/device_files.js`, `node_modules/@esp-vision/device-core/src/device_python.js`, `node_modules/@esp-vision/device-core/src/firmware_feed.js`, `node_modules/@esp-vision/device-core/src/firmware_flash.js`, `node_modules/@esp-vision/device-core/src/firmware_io.js`, `node_modules/@esp-vision/device-core/src/firmware_release.js`, `node_modules/@esp-vision/device-core/src/index.js`, `node_modules/@esp-vision/device-core/src/network_discovery.js`, `node_modules/@esp-vision/device-core/src/network_discovery_node.js`, `node_modules/@esp-vision/device-core/src/node.js`, `node_modules/@esp-vision/device-core/src/node_inventory.js`, `node_modules/@esp-vision/device-core/src/node_rom.js`, `node_modules/@esp-vision/device-core/src/node_transport.js`, `src/cli.js`

```text
// SPDX-License-Identifier: Apache-2.0
```
