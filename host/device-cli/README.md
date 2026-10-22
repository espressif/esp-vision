# ESP-VISION Device CLI

`@esp-vision/device-cli` installs the `esp-vision-device` command for device discovery, inspection, configuration, control, firmware updates, Python execution, device file operations, JPEG capture, sequential batch plans, and bounded event watches. It is an optional host interface backed by the same `@esp-vision/device-core` package used by ESP-VISION clients.

Requires Node.js 22 or newer. Serial access requires Python with `pyserial`; firmware flashing requires Python `esptool>=5.3,<6`. LAN discovery uses the bundled multicast-DNS client.

Run `esp-vision-device --help` for the command reference, or `esp-vision-device --help --json` for the versioned machine-readable command, output, and limit capabilities envelope.

## Python execution

Choose exactly one input: `--source CODE` for inline code, `--file LOCAL.py` for a local source file, or `--path DEVICE.py` for a file already on the device. `--timeout MS` must be a positive integer and cancels a waiting execution when it expires. `--max-output-chars` bounds captured Python output.

The default `--completion wait` waits for completion and returns bounded output metadata. `--completion start` requires `--path` and acknowledges after the device VM enters its runner. Omit `--timeout` with `start`; it is an execution deadline for `wait` only. This does not confirm that the application initialized successfully or completed. The CLI then releases the connection and exits; it does not keep watching or resume the script.

During foreground `python` execution and Python steps in a batch, `python.output`, `python.log`, and `python.state` events stream as NDJSON on stderr while the command is still running. Python text is capped by `--max-output-chars` (16,384 by default); a single event with `truncated: true` marks the limit. The stdout JSON summary contains metadata, not a duplicate of streamed output. For live Python output, use `--completion wait`.

## Runtime state and connection behavior

A targeted `inspect`, `configure`, firmware check, `capture`, `watch`, or native `files write` acquires the device without probing the friendly REPL, so these operations do not send Ctrl-C to a running application. `inspect --probe` actively checks discovered ports using the same passive connection. Python execution, REPL-backed `files list/read/remove/mkdir`, `control`, firmware updates, and batches use the normal REPL connection probe; these commands can send Ctrl-C.

The `runtime.scriptRunning` and `runtime.run` fields describe work tracked by the current SDK session. A fresh CLI process cannot use them to determine whether a detached program started earlier is still running on the device. `runtime.vmReady` means the VM is ready; it does not mean the VM is idle.

## Device files and capture

For a continuous app, finish directory operations and uploads before starting it, then take passive captures while it runs. Stop it with `control --action stop` before reading progress files. `--mode soft|hard` applies only to `control --action reset`; it is not a stop option. A REPL-backed read can interrupt the app and trigger LCD cleanup, so a subsequent `DISPLAY_NOT_READY` does not by itself establish a firmware failure. Use increasing application frame counters across captures to verify liveness; a fresh connection's session-local execution state cannot establish global idleness.

`files list`, `files read`, `files write`, `files remove`, and `files mkdir` operate on device paths. `files read DEVICE_PATH --output LOCAL_FILE` writes binary content to a local file. `files write DEVICE_PATH --file LOCAL_FILE` sends local bytes to the device. Reads are capped at 16 MiB by default; `--max-bytes` can lower that limit.

`capture camera|screen --output LOCAL.jpg` writes one JPEG and returns its path, byte count, source, MIME type, and capture metadata. Images are never embedded as base64 in JSON output.

## Bounded event watch

`watch --duration MS --port PATH` or `watch --duration MS --tcp HOST[:PORT]` keeps one device connection open for the requested duration (1 ms to 1 hour). It passively observes Python output, device logs, state changes, and diagnostics available to the currently acquired session as NDJSON on stderr, then releases the connection and returns a JSON summary. EV-MUX pins Python stdout to the session that started the program, so a new watch cannot reattach to stdout from a detached `--completion start` run. Use foreground `--completion wait` when live Python output is needed. To check a detached app, use captures or have the app write progress artifacts that can be read later. SIGINT and SIGTERM cancel the watch and still release ownership. A lost device connection is reported as a command failure; the CLI does not reconnect or launch a script.

## Batch plan

`batch PLAN.json --port PATH` or `batch PLAN.json --tcp HOST[:PORT]` validates the complete plan before connecting, then runs each operation in order over one connection. Local `file` and `output` paths are relative to the plan file. The first failed operation stops the plan; the report contains completed operations and the failure. The device is disconnected in all cases.

```json
{
  "schemaVersion": 1,
  "operations": [
    { "op": "python", "source": "print('hello')" },
    { "op": "files.list", "path": "/" },
    { "op": "files.read", "path": "/boot.py", "output": "./boot.py" },
    { "op": "files.write", "path": "/backup.bin", "file": "./backup.bin" },
    { "op": "files.remove", "path": "/old.bin" },
    { "op": "files.mkdir", "path": "/data", "recursive": true },
    { "op": "capture", "source": "camera", "output": "./frame.jpg", "quality": 80 }
  ]
}
```

## Output and cancellation

`--json` writes exactly one JSON envelope to stdout with `schemaVersion: 1`. A success uses `{ "schemaVersion": 1, "ok": true, "command": "...", "result": ... }`. A failure uses `{ "schemaVersion": 1, "ok": false, "command": "...", "error": { "code": "...", "message": "..." } }`; cleanup failures appear separately as `cleanupError`. Batch failures include `completed` and `failed` entries. Binary data is written to files, never embedded in the envelope.

Status, Python output, firmware phases, cancellation, errors, and duration are NDJSON events on stderr. SIGINT exits with code 130, SIGTERM with 143, argument/plan errors with 2, other failures with 1, and success with 0. Busy or uncertain operations are never retried automatically.

Inspect JSON output uses the same versioned envelope as other commands. Human-readable inspect output remains a terminal table.
