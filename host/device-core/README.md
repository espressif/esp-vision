# ESP Vision Device Core

[ESP-VISION](https://vision.espressif.com/) is Espressif's low-code edge AI and computer-vision framework for ESP32 boards. MicroPython APIs cover camera capture, image processing, inference and display on the device; browser flashing and the Web IDE provide the host development workflow. Device Core is its reusable, UI-independent host communication library.

- Project GitHub: [espressif/esp-vision](https://github.com/espressif/esp-vision)
- Official website: [vision.espressif.com](https://vision.espressif.com/)
- Web IDE: [ide.vision.espressif.com](https://ide.vision.espressif.com/)
- Firmware documentation: [ESP-VISION programming guide](https://docs.espressif.com/projects/esp-vision/en/latest/)

## Package identity and installation

| Item | Value |
| --- | --- |
| Published name | `@esp-vision/device-core` |
| First public version | `0.1.0`, EV-MUX v4 only |
| Registry | `https://registry.npmjs.org/` |
| Package page | [npm: @esp-vision/device-core](https://www.npmjs.com/package/@esp-vision/device-core) |
| Publishing account | [`esp-vision`](https://www.npmjs.com/~esp-vision) |
| Visibility | Public; installation needs no npm login |
| Source location | `host/device-core` in the ESP-VISION firmware repository |

The npm page and installation become available only after the first publication succeeds. CI authenticates with the protected `NPM_PUBLISH_TOKEN` and verifies that `npm whoami` returns `esp-vision` before publishing. Tokens are not part of this package or its documentation.

```sh
npm install --save-exact @esp-vision/device-core@0.1.0 --registry=https://registry.npmjs.org/
```

EV-MUX v4 firmware is required. A v3 hello is rejected with `VERSION_MISMATCH`; applications should ask the user to update firmware. Core has no runtime dependency on the IDE, Agent, DOM or Node serial libraries. The host supplies the physical byte transport and discovery adapter; Core owns protocol parsing, session negotiation, REPL coordination and device operations.

## Documentation

Read the Device Core guide in the **ESP-VISION Documentation Center**: [English](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/index.html) | [简体中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/device-core/index.html).

- [All application APIs](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/api.html), including firmware version checks and updates.
- [Advanced exports](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/advanced.html) and [TCP/Web Serial integration](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/integration.html).

Documentation source lives in this repository under `docs/en/device-core/` and `docs/zh_CN/device-core/` and changes alongside SDK APIs in the same MR. The documentation site renders the RST as HTML; it does not redirect readers to npm or require downloading documentation. The npm package contains the runtime library and this README, not a second RST documentation distribution. Use the documentation center's version selector when reading a published firmware/documentation version; `latest` follows the current development documentation. These new chapter links become available after the corresponding documentation is deployed.

## Management CLI

The npm package installs `esp-vision-device`. `inspect` combines passive wired/LAN enumeration and targeted queries. By default it lists USB serial candidates; `--all-ports` also includes native UART/Bluetooth ports. Explicit `--port` bypasses discovery filtering; `configure`, `control`, and `firmware check/update` cover management. See the [CLI guide](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/cli.html) ([中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/device-core/cli.html)).

```sh
npm install -g @esp-vision/device-core@0.1.0
esp-vision-device inspect
esp-vision-device inspect --all-ports
esp-vision-device inspect --interface usj,usb --probe --json
esp-vision-device inspect --port COM3 --fields identity,capabilities
esp-vision-device inspect --tcp 192.168.1.100:3232 --fields network,storage --json
```

Wired access/enumeration requires Python with pyserial (`--python` overrides the interpreter); LAN discovery uses optional `multicast-dns`, normally installed by npm. Windows/Linux/macOS paths are accepted. Passive enumeration never opens a serial port; `--probe` and targeted queries acquire a session and may interrupt Python. Unidentified ports, ROM candidates and USB-to-UART adapters remain numbered and visible. Board names are unknown until firmware identification; USB descriptors are hints only. Firmware versions are fetched automatically from dl.espressif.com. Firmware flashing uses the built-in serial adapter with separately installed Python esptool>=5.3,<6; no user loader module is required. Historical releases without qualified update metadata remain listed but cannot be installed by this flow. Installation becomes available after first npm publication.

## Public device API

Import `createDevice` from the package root. Application code uses one device interface rather than constructing parsers, a REPL and a session separately.

| Area | Method | Responsibility |
| --- | --- | --- |
| Discovery | `discover(options)` | List LAN advertisements without acquiring devices |
| Connection | `connect(options)`, `disconnect()` | Negotiate/release an EV-MUX session and manage the injected transport |
| Query | `inspect({ fields })` | Read identity, capabilities, runtime, network and storage state |
| Configuration | `configure({ network })` | Change Wi-Fi credentials and enabled state |
| Control | `control({ action, mode })` | Stop Python, reset or enter the ROM downloader |
| Interactive Python | `python.input(data)` | Send terminal input without uploading a file |
| Python execution | `python.execute({ source, path, completion })` | Execute source or a device-side script |
| Files | `files.list/read/write/remove/mkdir(request)` | File and directory operations, with one object argument |
| Firmware | `firmware.check(request)`, `firmware.update(request)` | List board firmware versions, select an exact version and perform verified ROM application updates |
| Capture | `capture({ source, format, quality })` | Capture an image from a supported source |
| Events | `on(event, handler)` | Subscribe; returns an unsubscribe function |

Configuration and control are deliberately separate: configuration changes persistent settings, while control requests an immediate action. File upload never implicitly starts Python execution.

### Transport and connection

The host owns serial/TCP/WebSocket details and implements this byte transport contract:

```js
const transport = {
  async writeBytes(bytes) {
    // Resolve only after all bytes are accepted; reject on failure.
    await byteConnection.write(bytes);
  },
  subscribe({ data, error, close }) {
    // Forward received Uint8Array data and link errors/closure.
    // Return a synchronous unsubscribe function; do not make this async.
    return byteConnection.subscribe({ data, error, close });
  },
  async connect({ signal }) { await byteConnection.open({ signal }); }, // Optional.
  async disconnect() { await byteConnection.close(); },   // Optional.
};
```

`byteConnection` above represents your host adapter, not a Core class. Core subscribes before opening the connection or sending handshake bytes. If the host opens the connection itself, omit `connect()` and pass the open adapter. A closed connection terminates pending work; unsubscribe prevents further input from an old connection.

```js
import { createDevice } from '@esp-vision/device-core';

const device = createDevice({ transport });
const unsubscribeOutput = device.on('python.output', text => console.log(text));
const unsubscribeState = device.on('state', state => console.log(state));
try {
  await device.connect({ role: 'usj' }); // 'usb', 'uart' or 'tcp' on other links.
  console.log(await device.inspect({ fields: ['identity', 'capabilities'] }));
  console.log(await device.python.execute({ source: 'print(1)', completion: 'wait' }));
} finally {
  await device.disconnect();
  unsubscribeOutput();
  unsubscribeState();
}
```

A discovery-only instance can receive a transport later:

```js
const [selected] = await device.discover({ timeout: 3000 });
await device.connect({ transport: tcpAdapter, role: 'tcp', expectedDevice: selected });
```

Opening `tcpAdapter` from the selected address/bridge URL remains the host's responsibility. Supported query fields are `identity`, `capabilities`, `runtime`, `network` and `storage`. The default is identity/capabilities/runtime; `inspect` reads only the requested fields and does not change configuration. Network/storage queries require the corresponding firmware capabilities. `connect()` returns that default inspection result after negotiation.

### Events

`on(name, handler)` returns an unsubscribe function. Register output handlers before connecting if you need to see startup output.

| Event | Payload |
| --- | --- |
| `python.output` | A string containing Python stdout/stderr text or interactive REPL prompts |
| `raw` | A string containing text outside EV-MUX framing |
| `log` | A string containing firmware diagnostic log text |
| `preview` | `{ data: Uint8Array, mimeType: 'image/jpeg', metadata }` |
| `state` | `{ state, identity?, reason? }`, including `connecting`, `ready` and `disconnected` |
| `diagnostic` | An object with `event` and diagnostic details; includes `rpc.request`, `rpc.response`, `rpc.timeout` |
| `rpc` | An inbound RPC object `{ channel, metadata, payload }` |

Preview frames are delivered as events, while `capture()` returns the requested one-shot image. Diagnostic event payloads are for troubleshooting, not a replacement for query/configuration methods. Network credentials are redacted from RPC diagnostics; applications should still avoid logging their own configuration requests. A throwing event listener cannot interrupt parser delivery or other listeners.

### Configuration and control

Network configuration uses a partial update: an omitted property stays unchanged. `credentials: null` clears saved credentials and disables networking. Combining it with `enabled: true` is rejected. Supplying credentials requires both `ssid` and `password`; use an empty password for an open network.

```js
await device.configure({
  network: {
    enabled: true,
    credentials: { ssid: 'example', password: 'secret' },
  },
});
await device.configure({ network: { enabled: false }, timeout: 45000 }); // Keep credentials.
await device.configure({ network: { credentials: null } });

await device.control({ action: 'stop' });
await device.control({ action: 'reset', mode: 'soft' });
await device.control({ action: 'reset', mode: 'hard' });
await device.control({ action: 'bootloader' });
```

A combined configuration request is applied in sequence, not as a transaction. If it fails, `error.completed` describes acknowledged changes and `error.pending` identifies an unfinished operation; Core does not roll settings back. Pass `timeout` and `signal` in the same request object as `network`/`action`, not as a second options argument. Configuration timeout is the budget for the combined operation. Wi-Fi changes may disconnect a TCP connection. Hard reset and entering the downloader can also remove USB interfaces; reconnect through the host transport after the device returns. A successful reset request does not guarantee that the rebooted firmware is already online. Configuration does not return saved Wi-Fi passwords.

### Interactive Python and scripts

```js
const unsubscribe = device.on('python.output', text => terminal.write(text));
await device.python.input('print(1)\r');
await device.control({ action: 'stop' }); // Ctrl-C / stop.

const result = await device.python.execute({
  source: 'print(1)',
  completion: 'wait',
});
await device.python.execute({ path: '/main.py', completion: 'start' });
unsubscribe();
```

`source` and `path` are mutually exclusive. `completion: 'wait'` (the default) returns `{ ok, path, output, outputTruncated }` after the program finishes. `'start'` returns `{ ok, path, done }` after starting it; await `done` for the execution result. Source started this way uses an internal temporary script that Core removes after execution. Terminal input, scripts and filesystem helpers share one MicroPython interpreter. The application should not send terminal input while an automated operation owns the REPL. Conflicting automated operations reject with `DEVICE_BUSY`; Core does not silently queue them or isolate user Python in a second interpreter. Terminal input remains available to an explicitly started interactive script.

### Files and images

```js
await device.files.mkdir({ path: '/models' });
await device.files.write({
  path: '/models/model.espdl',
  data: modelBytes, // Uint8Array; strings are UTF-8.
  overwrite: true,
  signal: abortController.signal,
  onProgress: progress => console.log(progress),
});
const bytes = await device.files.read({ path: '/models/model.espdl' });
const entries = await device.files.list({ path: '/' });
await device.files.remove({ path: '/models/model.espdl' });

const image = await device.capture({ source: 'camera', format: 'jpeg', timeout: 15000 });
// image: { data: Uint8Array, mimeType: 'image/jpeg', metadata }
```

Paths are absolute device paths. `files.list({ path: '/', recursive: false })` returns entries `{ name, path, type, size }`. `files.read({ path })` returns a `Uint8Array`; use `encoding: 'utf-8'` to return text, and `maxBytes` to bound a read. Writes default to `overwrite: true`; set it to `false` to require a new destination. Uploading and executing are separate operations; create an empty file with `files.write({ path, data: new Uint8Array() })` rather than a second touch API. Remove an empty directory with `files.remove({ path: '/models', type: 'directory' })`; there is no implicit recursive deletion. `files.mkdir({ path, recursive: true })` creates missing parents.

**Current firmware limits:** file upload still uses the existing `script.write` chunked RPC, including Base64/JSON overhead. This API cleanup does not implement the proposed high-throughput raw binary protocol or promise improved model-upload throughput. The filesystem must be mounted and allow writing, with room for the temporary upload and any existing destination file. File reads and directory operations currently use the REPL.

`capture({ source: 'screen', format: 'jpeg' })` uses the dedicated `debug.capture_screen` RPC to encode the LCD's last submitted RGB565 framebuffer, including the display's scaling and letterboxing. It does not read the panel electronics or capture the camera. Camera capture uses `debug.capture_frame`. Both return binary JPEG responses correlated to the request; optional `quality` is an integer from 1 to 100. Missing firmware capabilities reject with `UNSUPPORTED_OPERATION`; an uninitialized display returns `DISPLAY_NOT_READY`, and a board without an LCD returns `NOT_SUPPORTED`. Preview is a separate real-time stream and may drop frames under congestion.

## Discovery and transport adapters

Firmware advertises `_esp-vision._tcp.local` with the main-chip device ID, full board name, firmware version, canonical hostname and EV-MUX version. The hostname is `esp-vision-<full-board-name>-<last-6-mac-hex>-<firmware-version>`; all transport queries return the same name. Discovery does not open a device TCP socket or acquire ownership. Occupancy is established during connection.

Node discovery uses the optional `multicast-dns` peer:

```js
// npm install multicast-dns
import { createDevice } from '@esp-vision/device-core';
import { createNodeDiscovery } from '@esp-vision/device-core/discovery/node';

const device = createDevice({ discovery: createNodeDiscovery() });
const devices = await device.discover({ timeout: 3000 });
```

Browser discovery uses an HTTP endpoint on a local bridge:

```js
import { createDevice } from '@esp-vision/device-core';
import { createBridgeDiscovery } from '@esp-vision/device-core/discovery';

const device = createDevice({
  discovery: createBridgeDiscovery('ws://127.0.0.1:8766/device'),
});
const devices = await device.discover({ timeout: 3000 });
```

Results contain `deviceId`, `hostname` (without `.local`), `board`, `firmwareVersion`, `evMuxVersion`, `compatible`, `addresses`, `port`, and adapter-specific `expiresAt`/`connectionUrl`. Supply the selected result as `expectedDevice` when connecting: Core checks the stable ID and hostname before acquisition, protecting against stale DHCP addresses. This is identity matching, not authentication. Refresh expired bridge handles. Multicast filtering, VLANs and client isolation may require a manual address. Browsers have no native DNS-SD enumeration API; local-network permission does not remove the bridge requirement.

The root/browser entry points do not import Node networking modules. The optional Node discovery subpath dynamically loads `multicast-dns`. No Agent package is required.

## Migration from the session API

This is the API boundary for the first public v4 release. The package root exports only `createDevice()`. The `Device` class is an internal implementation detail. Legacy implementation classes are not exported there. Protocol adapters and existing integrations can use explicit subpaths such as `/session`, `/evmux`, `/evrepl`, `/actions` and `/bindings` while migrating; new application code should use `createDevice`.

| Previous application call | Device API |
| --- | --- |
| Construct `DeviceSession` with `EvMuxParser`, `EvRepl`, `makeEvMuxFrame` | `createDevice({ transport, discovery })` |
| `NetworkDiscovery.list()` | `device.discover(options)` with an injected discovery provider |
| `negotiate()`, `release()`, `close()` | `connect()`, `disconnect()` |
| `snapshot()`, `hasFeature()`, `networkStatus()`, REPL information helpers | `inspect({ fields })` |
| `configureNetwork()`, `setNetworkEnabled()`, `forgetNetwork()` | `configure({ network })` |
| `interrupt()`, `stopForRepl()`, `control('soft-reset')` | `control({ action, mode })`; readiness barriers are internal |
| `repl.writeStdin()` | `python.input(data)` |
| `executePython()`, `runScript()`, `startScript()` | `python.execute({ source OR path, completion })` |
| `writeFile()`, `writeScript()`, `repl.writeFile()` | `files.write({ path, data, overwrite, signal, onProgress })` |
| `repl.walkFs/readFile/removeFile/removeDir/makePath()` | `files.list/read/remove/mkdir(request)` |
| `repl.touchFile()` | `files.write({ path, data: new Uint8Array() })` |
| `handlers.text/rawText/preview/log/...` | `on(event, handler)` |
| Action `frame.capture` | `capture({ source: 'camera' })` |
| Root firmware-flashing exports | `@esp-vision/device-core/firmware` |

IDE and Agent adapters that construct sessions or call `session.repl` must migrate at their own integration boundary. Do not implement a second parser or file-transfer protocol in the UI. Existing action contracts and protocol classes remain adapter implementation tools rather than a second application API.

## Firmware flashing

ROM flashing remains a separate lifecycle under `@esp-vision/device-core/firmware`. Hosts inject the ROM loader, approved image manifest and reset policy. The adapter validates image integrity, chip/revision, security and partitions, verifies complete readback, and clears the P4 download latch before reset. It does not distribute firmware, open a browser picker or provide a flashing UI. EV-MUX `control({ action: 'bootloader' })` only requests entry into the downloader; it does not flash an image.

Agent workflows, MCP tool mappings, model providers, CLI bundles and plugin packaging belong to the independent Agent project and are not included here.

## Validation and release

From this directory, using Node.js 22:

```sh
npm test
npm run check:schema
npm run check:package
npm pack
```

Set `TMPDIR`, `TMP` and `TEMP` to a project-local temporary directory when running package verification. The package check installs a tarball in an isolated temporary directory and imports public entry points. Node.js 22 is the supported Node host baseline. Browser hosts need Web Serial or a binary WebSocket-to-TCP bridge plus a bundler/import map for the package specifier.

Device Core is maintained alongside the firmware at `host/device-core` in [espressif/esp-vision](https://github.com/espressif/esp-vision). Protocol changes, host implementation, documentation and contract tests are reviewed together. SDK versions remain independent of firmware versions; compatibility is negotiated through EV-MUX v4 capabilities. IDE and Agent consume the same npm package and do not maintain protocol copies.

Run `npm run release:check` from this directory for unit tests, schema validation, an isolated package install and a publication dry run. SDK-only changes run these Node.js checks without compiling firmware. `ESP_VISION_DEVICE_CORE_ONLY=1` selects SDK checks in an API/web pipeline; `ESP_VISION_BUILD_FIRMWARE=1` requests the final firmware matrix when needed. SDK versions are tracked in `package.json` and npm; do not create SDK Git tags in this firmware repository, because firmware/documentation version discovery uses Git tags. CI defensively excludes accidental `device-core-v*` tags from firmware jobs, but creating these tags is not part of the SDK release workflow.

Only a protected default-branch push (`master` in this firmware repository) with matching SDK changes can publish, using the masked/protected `NPM_PUBLISH_TOKEN`. The account must be `esp-vision` with scope write access. This variable must be configured in the firmware project before its first publication; moving source does not transfer CI credentials. MR, feature-branch, local and SDK-tag runs never publish. The release job verifies registry metadata and integrity: identical content is skipped; different content under an existing version requires a version bump.

Before enabling this publisher, disable the old IDE-project package release job so there is only one authoritative publisher. Migration is staged: qualify this in-repository SDK and its publication pipeline, publish and verify its registry integrity, then update the IDE/Agent adapters to the fixed package version and remove the old IDE source copy. Until that consumer migration is complete the old worktree is preserved as a backup, not an additional source of future SDK changes. No npm publication is performed by local migration tests.

The package includes library source, schemas, license and documentation, with no IDE/Agent bundles or release scripts. Every future release must update its version and changelog.

## Hardware acceptance

Run the checked-out SDK against one idle device using Node.js 22. These checks use the public `createDevice()` API and release the session and transport afterwards. The default check reads identity/capabilities/runtime and executes a short `print()` without creating a device file. Connecting negotiates the REPL, so use a device whose running Python application may safely be interrupted. The check does not configure Wi-Fi, reset the board or erase storage.

```sh
npm run test:hardware -- --tcp 192.168.1.100:3232
npm run test:hardware -- --port /dev/serial/by-id/YOUR_DEVICE --role usj
npm run test:hardware -- --port /dev/serial/by-id/YOUR_USB_OTG_DEVICE --role usb --write
npm run test:hardware -- --tcp 192.168.1.100 --write --capture --timeout 90000
npm run test:hardware -- --port /dev/serial/by-id/YOUR_DEVICE --role usj --checks all --frames 30 --output camera.jpg
npm run test:hardware -- --tcp 192.168.1.100 --checks display --display-hold 2 --screen-output screen.jpg
```

`--write` explicitly opts into creating a randomly named 1 KiB binary file, verifying every byte by readback, and deleting that file and its temporary upload sibling. Existing matching paths are refused; cleanup failures are reported with the test path and make the command fail. `--capture` opts into a camera JPEG check; `--output` optionally saves that JPEG. SIGINT/SIGTERM cancel the operation; cleanup has a separate bounded deadline. Output excludes network credentials, arbitrary device logs and Python tracebacks. A failing check exits nonzero.

`--checks all` runs the shared firmware/SDK hardware suite: `identity`, `memory`, `camera`, `framesizes`, `pixformats`, `orientation`, `ipa_stats`, `snapshot_rate`, `preview`, `image_ops`, `display`, and `display_stream`. Select a comma-separated subset with `--checks memory,camera,preview`. Timing controls are `--frames` (default 30), `--frame-timeout` (seconds, default 25), and `--display-hold` (seconds per backlight state, default zero). Sensor defaults are `--framesize QVGA --pixformat RGB565`; `--quality` defaults to 60. Functional checks default to a 600000 ms overall deadline; an explicit `--timeout` overrides it. Each result is a JSON line with PASS/FAIL/SKIP, measurements and duration, followed by a summary. Unsupported hardware is skipped; IPA absence remains a failure on boards whose camera requires IPA.

These functional checks intentionally exercise sensor settings, image processing, preview and the LCD. Their single Apache-2.0 MicroPython fixture is staged to a unique device path through `files.write()`, loaded through `python.execute()`, and removed. Subsequent checks use short `python.execute()` calls; preview reception uses the public event API. The suite removes its fixture and Python namespace, releases its display objects and disconnects. It does not overwrite application files or maintain a second EV-MUX/RPC implementation.

`--screen-output PATH` explicitly requests an LCD screenshot: it adds the `display` check when needed, draws red/green/blue/white bars with `ESP-VISION CI` text, validates backlight changes, captures that same display with `capture({ source: 'screen' })`, verifies JPEG completeness and panel dimensions, and writes the JPEG on the host before releasing the display. Missing LCD or screen capture support makes this explicit request fail instead of skip. Camera capture stays a separate source. Output images and logs should be placed under your project-local artifact directory when running this suite.

TCP needs only Node.js. Serial additionally needs a Python interpreter with the optional [pyserial](https://pyserial.readthedocs.io/) dependency installed in your environment (`python3 -m pip install pyserial`, BSD-3-Clause; not vendored or included in the npm package). Select another interpreter with `--python /path/to/python`. The Apache-2.0-licensed serial adapter in `src/node_serial.py` is shared by the CLI and hardware runner; it enumerates ports without opening them, forwards bytes and configures RTS/DTR before opening; serial control-line behavior still depends on the board and OS driver. Hardware-check scripts and their tests stay in the source tree and are excluded from the published package.

## License

The npm package and ESP-VISION contributions use [Apache-2.0](LICENSE), matching the firmware. [NOTICE](NOTICE) retains the original copyright and MIT permission text for historical and upstream-derived portions. Third-party dependencies retain their own licenses; neither package metadata nor the project license overrides them.
