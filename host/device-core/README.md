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

This source targets **0.8.0**, the npm serial/backend migration. Device Core is SDK-only: the management CLI lives in `@esp-vision/device-cli`, and Python execution/file reads have no aggregate deadline unless configured. Declare a minor-locked dependency and use a lockfile to record the resolved patch and integrity; verify the chosen version exists in the public registry before installing. For unpublished source changes, use a tarball packed from an exact reviewed source commit. CI authenticates with the protected `NPM_PUBLISH_TOKEN` and verifies that `npm whoami` returns `esp-vision` before publishing. Tokens are not part of this package or its documentation.

```sh
# Published baseline; the pending 0.7.1 fixes require a candidate tarball.
npm view @esp-vision/device-core@0.7.0 version dist.integrity
npm install '@esp-vision/device-core@~0.7.0' --registry=https://registry.npmjs.org/
```

A source version, dry run or open merge request does not publish a package. CLI and Plugin bundles keep their own versions and embedded Core bytes; publishing Core alone does not update those installed tools.

EV-MUX v4 firmware is required. A v3 hello is rejected with `VERSION_MISMATCH`; applications should ask the user to update firmware. Core has no runtime dependency on the IDE, Agent, DOM or Node serial libraries. The host supplies the physical byte transport and discovery adapter; Core owns protocol parsing, session negotiation, REPL coordination and device operations.

## Documentation

Read the Device Core guide in the **ESP-VISION Documentation Center**: [English](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/index.html) | [简体中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/device-core/index.html).

- [All application APIs](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/api.html), including firmware version checks, application updates and blank-device installation.
- [Advanced exports](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/advanced.html) and [TCP/Web Serial integration](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/integration.html).

Documentation source lives in this repository under `docs/en/device-core/` and `docs/zh_CN/device-core/` and changes alongside SDK APIs in the same MR. The documentation site renders the RST as HTML; it does not redirect readers to npm or require downloading documentation. The npm package contains the runtime library and this README, not a second RST documentation distribution. Use the documentation center's version selector when reading a published firmware/documentation version; `latest` follows the current development documentation. These new chapter links become available after the corresponding documentation is deployed.

## Management CLI

Starting with 0.5.0, Device Core is a library and does not install a management command. Existing 0.4.x installations retain their command until explicitly upgraded or removed. Install the independent `@esp-vision/device-cli` package when you need the device toolkit. It uses the public Core APIs and is installed explicitly; Device Core does not fetch or wrap the CLI. See the [CLI guide](https://docs.espressif.com/projects/esp-vision/en/latest/esp32p4/device-core/cli.html) ([中文](https://docs.espressif.com/projects/esp-vision/zh_CN/latest/esp32p4/device-core/cli.html)).

```sh
npm install -g @esp-vision/device-cli
esp-vision-device inspect
esp-vision-device inspect --all-ports
esp-vision-device inspect --interface usj,usb --probe --json
esp-vision-device inspect --port COM3 --fields identity,capabilities
esp-vision-device inspect --tcp 192.168.1.100:3232 --fields network,storage --json
```

Wired access/enumeration uses optional npm native serial bindings; LAN discovery uses optional `multicast-dns`, normally installed by npm. Windows/Linux/macOS paths are accepted. Passive enumeration never opens a serial port; `--probe` and targeted queries acquire a session and may interrupt Python. Unidentified ports, ROM candidates and USB-to-UART adapters remain numbered and visible. Board names are unknown until firmware identification; USB descriptors are hints only. Firmware versions are fetched automatically from dl.espressif.com. Firmware flashing uses the same esptool-js engine as the browser; no user loader module is required. Use `firmware update --mode install --board BOARD --port PORT --version VERSION` for blank-device installation over a ROM-capable USJ/UART port. Install replaces data within the complete image range; default updates preserve the existing layout. Historical releases without qualified metadata remain listed but cannot be installed by this flow. Check the registry version before installing a newly prepared release.

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
| Firmware | `firmware.check(request)`, `firmware.update(request)` | List board firmware versions, select an exact version and perform verified ROM application updates or explicit complete installation on blank devices |
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

`connect()` sends Ctrl-C and a short REPL liveness probe by default for compatibility. Pass `probeRepl: false` when a detached application may be running: Core still acquires the EV-MUX session and reads identity/capabilities, but leaves the REPL untouched. In this mode a reported `vmReady: false` stays false; Python operations that require a ready VM fail with a readiness error.

UART connections start at 115200 bps. With compatible firmware and a transport exposing `setBaudRate(baudRate, { signal })`, Core tests 2000000, 1500000, 921600, 460800 and 115200 bps in order and stops at the first verified rate. `connect({ autoBaud: false })` skips this optional negotiation for bounded identity-only probes; `autoBaud` defaults to `true`. Native USB and legacy firmware skip UART negotiation. The connection deadline defaults to 60000 ms to allow bounded fallback. Runtime speeds and ROM/stub flashing speeds are negotiated independently.

The browser subpath exports a passive `serialTransport({ port, signals?, bufferSize? })` adapter. Request and authorize the selected `SerialPort` in the UI click handler, then pass it to this adapter. Core manages opening, internal retuning and closing; internal reopen preserves subscriptions and permission. The default stream buffer is 256 KiB. An unexpected board reset still requires protocol recovery; reopening a port alone does not establish a session.

### Immutable release snapshots and a single firmware BIN

The `/firmware` entry exports `loadOfficialFirmware({ board?, version?, fetch?, signal? })` and `loadReleaseManifest({ url, sha256, version?, sourceCommit?, boards?, fetch?, signal? })`. The official reader prioritizes `[publication]` in `config.toml`, selects exactly one version reference and verifies its raw manifest SHA-256 before returning `{ schemaVersion: 1, version, sourceCommit, pipelineId, layout, releases, website, mcp?, publication }`. History references include exact board sets; unsupported board/version combinations and malformed explicit publication data reject without falling back to guessed URLs or old metadata. Configs without `[publication]` keep the legacy feed; an explicit version limits old metadata requests too.

For a website built from a verified snapshot, keep its selected publication reference rather than re-reading mutable `config.toml` when the user clicks:

```js
import { loadReleaseManifest } from '@esp-vision/device-core/firmware';
const snapshot = await loadReleaseManifest({
  url: reference.manifestUrl, sha256: reference.manifestSha256,
  version: reference.version, boards: reference.boards,
});
const release = snapshot.releases.find(entry => entry.board === selectedBoard);
// Pass release to createDevice().firmware.update(); the website never slices BINs.
```

In `layout: 'single-bin'`, `image.source = { url, bytes, sha256, offset }` describes the complete BIN; `image.url` equals its URL, while `image.bytes/sha256` still describe only the application. The source URL/length/hash must match `install`, and its offset must equal the application's flash offset. Core downloads the full container once (no Range request), validates its whole hash and embedded bootloader/partition/application hashes, then extracts and writes only the application in default update mode. Existing partition checks and complete write verification remain mandatory; `mode: 'install'` writes the qualified complete BIN. With `image.source`, supplied `data` must also be the complete BIN; old application-only metadata keeps its existing raw-app `data` semantics. Manifests are bounded at 4 MiB, firmware downloads at 256 MiB and their approved exact size. Official manifest/artifact sources are confined to the ESP-VISION CDN root; model URLs use their full source commit and MCP has a version/commit/hash descriptor. No npm, firmware tag or CDN publication is performed by preparing this source version.

To validate before npm publication, check out an exact reviewed commit, run `npm install --ignore-scripts --no-audit --no-fund --package-lock=false` and `npm run release:check` in `host/device-core`, then `npm pack --ignore-scripts --pack-destination <project-local-output>`. Install that locally produced tarball or use a source archive pinned to that exact commit.

### Browser ROM flashing

`@esp-vision/device-core/browser` exports `openBrowserRom({ port, logger?, signal?, interface?, baudRate?, onStatus?, expectedMac? })`. Construction is passive; `initialize()` lazily loads the official `esptool-js@0.7.1` backend and synchronizes at 115200. Use a browser bundler such as webpack, Vite or esbuild to include the chip stubs. The root entry point remains platform-neutral; `/node` loads the shared ROM engine only on initialization. No Python installation or consumer-side dependency patch is needed.

The browser adapter introduced in 0.7.0 exposes `chipMac`, using the same 12 lowercase hexadecimal factory-MAC representation as the Node adapter. It is `null` before identity qualification. The browser reads it through the official chip's `readMac(loader)` primitive, without duplicating eFuse layouts. Pass the verified factory MAC of the selected runtime as `expectedMac` when handing that device to ROM, for example a validated `machine.unique_id()` known to match that board's factory MAC. The host must establish this source; an arbitrary legacy `deviceId` or USB VID/PID does not establish the association. If it cannot verify that value, use an explicitly user-selected manual ROM workflow rather than infer an automatic association. The option accepts exactly 12 hexadecimal characters, normalizes uppercase and rejects separators, whitespace, `null` and other types synchronously before opening a port. A mismatch returns `ROM_IDENTITY_CHANGED`, closes the adapter and cannot start a stub, erase or write. An unreadable/malformed MAC response returns `ROM_IDENTITY_INVALID`. Without the option, manually selecting an initial ROM remains supported; all later synchronization/recovery still binds the original family, revision and MAC, rejecting a different same-model device without rebinding.

`interface` accepts `auto` (default), `uart`, `usb-otg` and `usb-jtag`. Auto detection distinguishes known USB-UART bridge descriptors from Espressif OTG and Serial/JTAG; ambiguous custom descriptors stay conservative and can be identified explicitly. A ready OTG ROM is synchronized without toggling DTR/RTS. A detached or re-enumerated device requires selecting its current download port again; the SDK never treats a stale port object as a recovered session.

For UART, `baudRate: 'auto'` (default) tests **2000000 → 1500000 → 921600 → 460800 → 115200** after validating ROM security and starting the stub, then stops at the first speed that passes register and digest-checked read-only traffic. A numeric value from this list tests only that speed. A failed prewrite candidate resets, synchronizes at 115200, rechecks chip identity/security and reloads the stub before the next candidate. Native USB skips UART speed negotiation; the adapter's `baudRate` is `null` there. `onStatus(event)` reports ROM connection, speed testing/failure/selection, detected flash ID and compressed writing. Firmware writes are compressed and retain complete write verification and protected update bounds. Interrupted writes invalidate the session and require a fresh operation; the SDK never blindly resumes a compressed stream.

The bounded readback adapter for upstream issue [esptool-js #273](https://github.com/espressif/esptool-js/issues/273) uses 1 KiB packets, one outstanding packet and a checked trailing MD5 digest. This addresses protocol completeness without claiming a proven cause for the investigated read-after-write timing failures. Remove the adapter only after a pinned upstream replacement passes the same pre-read → complete write → complete readback tests. SHA-256 still qualifies source bytes before writing. Built-in adapters reuse official esptool device MD5 verification of the complete write; full SHA-256 readback remains the fallback for custom adapters without a write receipt.

```js
import { createDevice } from '@esp-vision/device-core';
import { openBrowserRom } from '@esp-vision/device-core/browser';

// Request permission directly in the user's click handler (HTTPS or localhost).
const port = await navigator.serial.requestPort();
// release comes from firmware.check(), selected for the physical board.
const result = await createDevice().firmware.update({
  release, board: release.board, mode: 'update', // Explicit 'install' for complete images.
  openLoader: ({ signal }) => openBrowserRom({ port, signal, logger: console }),
  onProgress: percent => updateProgress(percent),
});
if (result.needsReset) showManualReset();
```

Core owns downloads, chip/revision/security and partition/capacity checks, write, write verification and reset. The adapter owns the selected port until Core disconnects it; the UI must first release any runtime serial connection. It starts no stub during construction or chip initialization: Core checks the approved image and ROM security before calling `runStub`. P4 revision 301/302 flash power and S31 support come from pinned upstream 9.4.2; Core retains bounded SLIP-prefix tolerance, native USB stub startup confirmation and size-scaled erase timeouts. Unsupported stubs fail explicitly. Keep the page and port alive until the update Promise settles; cancellation is accepted before writing, never used to interrupt a started write/readback. This ROM adapter does not provide runtime EV-MUX serial transport or Wi-Fi/TCP OTA.

### Official Node adapters

The public Node entry also exports `inventory`, `listSerial`, `filterSerialPorts`, `serialEntry` and `transportFor`. Enumeration is passive unless `inventory({ probe: true })` is explicitly requested; cancellation is supported. Core 0.8 removes `defaultPython`, `nodeAdapterAssets` and the Python helper assets. Node bundlers must preserve the native npm dependencies and their prebuilt binaries; the standalone CLI build demonstrates this packaging.

The Node-only `@esp-vision/device-core/node` subpath exports `tcpTransport({ host, port })` and `serialTransport({ serial })`. Constructors are passive; Core opens/closes the link during `connect()`/`disconnect()`. Serial uses `@serialport/bindings-cpp@13.0.0`, with a Koffi 2.16.3 Linux USJ modem-control fallback, and role `usj`, `usb` or `uart`. The root/browser entry points do not load Node native dependencies.

```js
import { createDevice } from '@esp-vision/device-core';
import { tcpTransport, serialTransport } from '@esp-vision/device-core/node';

const tcpDevice = createDevice({ transport: tcpTransport({ host: '192.168.1.100', port: 3232 }) });
const usbDevice = createDevice({ transport: serialTransport({ serial: '/dev/ttyACM0', python: 'python3' }) });
// Connect only the selected device, and always disconnect() in finally.
```

A discovery-only instance can receive a transport later:

```js
import { createDevice } from '@esp-vision/device-core';
import { tcpTransport } from '@esp-vision/device-core/node';
import { createNodeDiscovery } from '@esp-vision/device-core/discovery/node';

const provider = createNodeDiscovery();
const device = createDevice({ discovery: provider });
const selected = (await device.discover({ timeout: 3000 })).find(item => item.compatible && item.addresses.length);
if (!selected) throw new Error('No compatible device found');
try {
  await device.connect({ transport: tcpTransport({ host: selected.addresses[0], port: selected.port }), role: 'tcp', expectedDevice: selected });
  console.log(await device.inspect({ fields: ['identity'] }));
} finally { await device.disconnect(); }
```

Application discovery is `device.discover()`. `createNodeDiscovery()` and `createBridgeDiscovery()` return providers whose adapter contract is `list({ timeout, signal })`; they do not expose `discover()`. Inject a provider into `createDevice({ discovery: provider })` rather than calling `provider.discover()`. `list()` is the lower-level integration method, not another device API.

Supported query fields are `identity`, `capabilities`, `runtime`, `network` and `storage`. The default is identity/capabilities/runtime; `inspect` reads only the requested fields and does not change configuration. Network/storage queries require the corresponding firmware capabilities. `connect()` returns that default inspection result after negotiation.

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

`source` and `path` are mutually exclusive. `completion: 'wait'` (the default) returns `{ ok, path, output, outputTruncated }` after the program finishes. `'start'` returns `{ ok, path, done }` after Core receives a private VM-entry marker from the generated runner; it does not wait for the program to finish, so a short script may already be done when the handle is returned. Core waits at most five seconds for this start acknowledgement. If it times out or is cancelled, the program may have entered and Core does not claim that it stopped. Await `done` for the execution result. Once the start handle is returned, `device.disconnect()` releases the session without requesting a stop; `done` rejects locally as the session closes, and the device program may continue. A caller-provided `signal` remains bound until `done` settles and requests a stop if aborted while the session is available. Source started this way uses an internal temporary script that Core removes after execution; disconnect can prevent this cleanup. Terminal input, scripts and filesystem helpers share one MicroPython interpreter. The application should not send terminal input while an automated operation owns the REPL. Conflicting automated operations reject with `DEVICE_BUSY`; Core does not silently queue them or isolate user Python in a second interpreter. Terminal input remains available to an explicitly started interactive script.

Small `source` requests with `completion: 'wait'` run through the existing REPL without creating a device file, so they work when storage is unmounted and do not require `script.run` or `file.write.binary`. Core holds the source in a temporary REPL variable, executes it in the normal global namespace so user variables remain available, and deletes only that temporary variable after completion. Source larger than the REPL input limit uses a temporary device script and requires `script.run`, `file.write.binary`, and writable storage.

For `completion: 'wait'`, omitting `timeout` disables the aggregate operation deadline; a positive `timeout` limits the full operation, including its REPL queue and startup exchanges. Core still bounds each REPL exchange and RPC, so an omitted total deadline does not create an unbounded protocol wait. If a timeout or cancellation occurs while code is running, its effects may be uncertain; Core does not claim that the program stopped or roll back device changes.

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

Paths are absolute device paths. `files.list({ path: '/', recursive: false })` returns entries `{ name, path, type, size }`. `files.read({ path })` returns a `Uint8Array`; use `encoding: 'utf-8'` to return text, and `maxBytes` to bound a read. Omitting `timeout` disables the aggregate read deadline; a positive `timeout` limits the full read. Each chunk request remains bounded by `chunkTimeout`, which defaults to 30 seconds. Writes default to `overwrite: true`; set it to `false` to require a new destination. Uploading and executing are separate operations; create an empty file with `files.write({ path, data: new Uint8Array() })` rather than a second touch API. Remove an empty directory with `files.remove({ path: '/models', type: 'directory' })`; there is no implicit recursive deletion. `files.mkdir({ path, recursive: true })` creates missing parents.

**Binary uploads (SDK 0.3.0):** text, model and image uploads share reliable raw binary `file.write` frames with 16 KiB blocks, continuous native VFS writes and SHA-256 commit. Matching firmware must advertise `file.write.binary`; missing support rejects with a firmware-upgrade message, without a slow upload fallback. File progress is not a durable commit. FAT replacement preserves an old-file backup until ordinary publication succeeds, with rollback on ordinary rename failure; it is not a power-loss-atomic journal. The filesystem must be mounted and allow writing, with room for the temporary upload and any existing destination file. File reads and directory operations currently use the REPL.

`capture({ source: 'screen', format: 'jpeg' })` uses the dedicated `debug.capture_screen` RPC to encode the LCD's last submitted RGB565 framebuffer, including the display's scaling and letterboxing. It does not read the panel electronics or capture the camera. Camera capture uses `debug.capture_frame` only when firmware also advertises `debug.capture_frame.observed.v1`. It observes the independent frame published after the application's last successful camera snapshot; it never initializes the sensor, acquires another physical frame or overwrites an application-held `Image`. Later application processing is not reflected in that independent observation. Missing safety support rejects locally with `UNSUPPORTED_OPERATION` and a firmware-upgrade message, without issuing the older capture RPC. No valid published frame, camera reconfiguration, shutdown or VM reset returns `CAMERA_FRAME_NOT_READY`; an observer allocation failure returns `NO_MEM` while the application snapshot remains successful. Use `connect({ probeRepl: false })` to observe a running application without the default REPL interruption. Both return binary JPEG responses correlated to the request; optional `quality` is an integer from 1 to 100. Missing firmware capabilities reject with `UNSUPPORTED_OPERATION`; an uninitialized display returns `DISPLAY_NOT_READY`, and a board without an LCD returns `NOT_SUPPORTED`. Preview is a separate real-time stream and may drop frames under congestion.

Capture and preview delivery require a binary, self-contained 8-bit Huffman JPEG (SOF0/1/2). After EV-MUX length/CRC and session correlation checks, Core checks marker and segment bounds, quantization and Huffman table structure/references, SOF dimensions against metadata, scan component coverage, nonempty scan/restart segments and a final EOI with no trailing bytes. This is structural validation without entropy decoding; it does not establish that every compressed coefficient can be decoded. Invalid capture responses reject immediately with `INVALID_IMAGE`; explicit firmware errors retain their original codes. Invalid preview frames are discarded before any image callback. Both emit a diagnostic `{ event: 'image.invalid', channel, method, seq, code: 'INVALID_IMAGE', reason }`; the parser's `invalidImageFrames` and `droppedFrames` counters increase. A stale response is filtered before validation and cannot reject another request.

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

Node-only adapters and passive serial inventory are exported from `@esp-vision/device-core/node`: `listSerial`, `filterSerialPorts`, `serialEntry`, `transportFor`, and `inventory`. `listSerial({ signal, timeout })` uses npm serial enumeration with a timeout and cancellation signal; it lists ports without opening them. `inventory()` lists without probing by default. Pass `probe: true` to acquire each selected EV-MUX session and query metadata; this does not send Ctrl-C or run a REPL probe, and reported `vmReady` state is preserved. Creating a transport with `transportFor()` is passive and does not open the port. Passive native USB ROM entries return `status: rom` without opening the port. Active `inventory({ probe: true })` first verifies the selected ROM identity/security, clears its software download latch and attempts normal SPI boot once, then probes only the matching MAC and physical USB location. `romProbe` records the transition; no returned runtime remains unidentified and may require physical BOOT/RESET before installation. ROM mode and timeouts do not prove firmware absence; serial/session ownership conflicts return `busy`.

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

ROM flashing remains a separate lifecycle under `@esp-vision/device-core/firmware`. Hosts inject the ROM loader, approved image manifest and reset policy. The adapter validates image integrity, chip/revision, security and partitions, verifies the complete write, and clears the P4 download latch before reset. It does not distribute firmware, open a browser picker or provide a flashing UI. EV-MUX `control({ action: 'bootloader' })` only requests entry into the downloader; it does not flash an image. Its framed REPL path has no device acknowledgement: `{ acknowledged: false, outcome: 'unknown' }` reports submission, and the next ROM connection must identify the chip before any write. Core invalidates the old EV-MUX lease after submitting a reset command or when that write has an uncertain outcome; disconnect does not send a stale `session.release`. A failure before the reset command is attempted retains the live lease for ordinary cleanup. The Node serial bridge drains its queued output before returning from a successful reset submission.

The Node loader resolves only the explicitly selected serial path and waits briefly for that path to become available. A selected ESP-VISION native USB runtime (`303a:4003`, or UVC composite `303a:4023`) must establish the active EV-MUX USB owner and match its device ID (or a legacy runtime's short `machine.unique_id()` probe) to its USB MAC serial before Core requests bootloader once. Core then accepts only one ROM interface with the same USB MAC and physical location, and verifies the ROM-reported factory MAC before allowing writes. Existing ROM, UART and USB Serial/JTAG ports are not probed as EV-MUX runtimes. If the first read-only ROM initialization fails before any handoff or write, Core may recover once only after freshly observing that same device back in its runtime and verifying the runtime again. It does not retry writes or repeat an already submitted bootloader request. `loader.usbTransition` records command acknowledgement, observed ROM enumeration and cleanup separately; `loader.port` is the qualified current OS path. Failed or cancelled initialization closes that loader and must not be replayed on the same instance. A missing old by-id path never authorizes guessing a different device.

Agent workflows, MCP tool mappings, model providers and plugin packaging belong to their own integration packages and are not included here.

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

A protected default-branch push (`master` in this firmware repository) with SDK changes automatically publishes a version not yet present in npm, using the masked/protected `NPM_PUBLISH_TOKEN`. The account must be `esp-vision` with scope write access. MR, feature-branch, local and SDK-tag runs never publish. An existing version is skipped even if source has changed; bump `package.json` before releasing those changes. Publication verifies newly published registry integrity.

Before enabling this publisher, disable the old IDE-project package release job so there is only one authoritative publisher. Migration is staged: qualify this in-repository SDK and its publication pipeline, publish and verify its registry integrity, then update the IDE/Agent adapters to the fixed package version and remove the old IDE source copy. Until that consumer migration is complete the old worktree is preserved as a backup, not an additional source of future SDK changes. No npm publication is performed by local migration tests.

The package includes library source, schemas, license and documentation, with no IDE/Agent bundles or release scripts. Every future release must update its version and changelog.

## Hardware acceptance

The package ships its unit suite and hardware runner. From your application directory, target the installed dependency explicitly:

```sh
npm --prefix node_modules/@esp-vision/device-core test
npm --prefix node_modules/@esp-vision/device-core run test:hardware -- --help
npm --prefix node_modules/@esp-vision/device-core run test:hardware -- --tcp 192.168.1.100:3232
```

Inside the source or installed package directory, use `npm test` and `npm run test:hardware -- ...`. Unit tests require no device. The runner explicitly enumerates tests and fails when the suite is absent or empty, rather than passing zero cases. The dedicated hardware runner uses `--serial`, as shown in its help.

Run the source or installed SDK against one idle device using Node.js 22. These checks use the public `createDevice()` API and release the session and transport afterwards. The default check reads identity/capabilities/runtime and executes a short `print()` without creating a device file. Connecting negotiates the REPL, so use a device whose running Python application may safely be interrupted. The check does not configure Wi-Fi, reset the board or erase storage.

```sh
npm run test:hardware -- --tcp 192.168.1.100:3232
npm run test:hardware -- --serial /dev/serial/by-id/YOUR_DEVICE --role usj
npm run test:hardware -- --serial /dev/serial/by-id/YOUR_USB_OTG_DEVICE --role usb --write
npm run test:hardware -- --tcp 192.168.1.100 --write --capture --timeout 90000
npm run test:hardware -- --serial /dev/serial/by-id/YOUR_DEVICE --role usj --checks all --frames 30 --output camera.jpg
npm run test:hardware -- --tcp 192.168.1.100 --checks display --display-hold 2 --screen-output screen.jpg
```

`--write` explicitly opts into creating a randomly named 1 KiB binary file, verifying every byte by readback, and deleting that file and its temporary upload sibling. Existing matching paths are refused; cleanup failures are reported with the test path and make the command fail. `--capture` explicitly prepares an application snapshot before checking passive camera observation; `--output` optionally saves that JPEG. SIGINT/SIGTERM cancel the operation; cleanup has a separate bounded deadline. Output excludes network credentials, arbitrary device logs and Python tracebacks. A failing check exits nonzero.

`--checks all` runs the shared firmware/SDK hardware suite: `identity`, `memory`, `camera`, `framesizes`, `pixformats`, `orientation`, `ipa_stats`, `snapshot_rate`, `preview`, `image_ops`, `display`, and `display_stream`. Select a comma-separated subset with `--checks memory,camera,preview`. Timing controls are `--frames` (default 30), `--frame-timeout` (seconds, default 25), and `--display-hold` (seconds per backlight state, default zero). Sensor defaults are `--framesize QVGA --pixformat RGB565`; `--quality` defaults to 60. Functional checks default to a 600000 ms overall deadline; an explicit `--timeout` overrides it. Each result is a JSON line with PASS/FAIL/SKIP, measurements and duration, followed by a summary. Unsupported hardware is skipped; IPA absence remains a failure on boards whose camera requires IPA.

These functional checks intentionally exercise sensor settings, image processing, preview and the LCD. Their single Apache-2.0 MicroPython fixture is staged to a unique device path through `files.write()`, loaded through `python.execute()`, and removed. Subsequent checks use short `python.execute()` calls; preview reception uses the public event API. The suite removes its fixture and Python namespace, releases its display objects and disconnects. It does not overwrite application files or maintain a second EV-MUX/RPC implementation.

`--screen-output PATH` explicitly requests an LCD screenshot: it adds the `display` check when needed, draws red/green/blue/white bars with `ESP-VISION CI` text, validates backlight changes, captures that same display with `capture({ source: 'screen' })`, verifies JPEG completeness and panel dimensions, and writes the JPEG on the host before releasing the display. Missing LCD or screen capture support makes this explicit request fail instead of skip. Camera capture stays a separate source. Output images and logs should be placed under your project-local artifact directory when running this suite.

TCP and serial need Node.js 22+. Serial requires the optional npm native dependencies enabled and a supported platform prebuild, plus OS device drivers and port permissions. The standalone CLI and plugins include their prebuilds; no Python subprocess or user-side compilation is used. The shared hardware runner, unit tests and device-side MicroPython fixture ship in the source tree and npm tarball. Firmware/build outputs and device credentials are excluded.

## License

The npm package and ESP-VISION contributions use [Apache-2.0](LICENSE), matching the firmware. [NOTICE](NOTICE) retains the original copyright and MIT permission text for historical and upstream-derived portions. Third-party dependencies retain their own licenses; neither package metadata nor the project license overrides them.

Hash-pinned manifests and complete-BIN sources use the browser HTTP cache (`cache: default`); mutable config and legacy app-only BINs retain `no-store`. A fetch-stage TypeError on a pinned asset gets one `no-store` retry to recover from an older cached response lacking CORS headers. HTTP, body-read, SHA-256 and metadata failures are not retried or downgraded.
