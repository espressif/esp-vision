# Continuous application verification

Use this reference when developing a continuous application or verifying live camera/LCD output. Resolve the installed plugin root and the user-selected device first. Read actual device storage before choosing a new application path; do not assume `/flash` or an SD card exists. Upload dependencies and finish directory operations before starting the app.

## Example sequence

The following POSIX shell example assumes the user selected a serial device and an application was uploaded to `/my-app/main.py`. Substitute the installed CLI and selected endpoint; keep output files in the user's workspace. For TCP, replace `--port` with the selected `--tcp` endpoint.

```sh
CLI=/absolute/installed/plugin/cli/device-cli.mjs
PORT=/dev/selected-device

node "$CLI" python --path /my-app/main.py --completion start --port "$PORT" --json
node "$CLI" capture screen --output first.jpg --port "$PORT" --json
# Capture again after enough time for the application to produce a new frame.
node "$CLI" capture screen --output second.jpg --port "$PORT" --json
node "$CLI" control --action stop --port "$PORT" --json
# Read a progress file only if the application actually creates one.
node "$CLI" files read /my-app/progress.json --output progress.json --port "$PORT" --json
```

`--completion start` accepts only a device path and does not accept an execution `--timeout`. Start acknowledgement means the VM entered the runner. Camera, LCD or model initialization may still be in progress or fail later. If a first capture returns `DISPLAY_NOT_READY`, allow a bounded initialization interval and retry the passive capture; preserve the error if readiness never arrives. Do not restart the app blindly after an unknown result.

`--mode soft|hard` belongs to `control --action reset`, not `stop`. Python `--completion wait` supports an optional execution `--timeout` when a bounded foreground run is needed.

## Observe without interrupting

| Operation | Effect on a running Python app |
| --- | --- |
| Explicit-device `inspect` without `--probe`, `capture`, `watch` | Passive observation |
| Native `files write` | Does not probe the REPL; use a distinct path and coordinate with any application writer |
| Python execution, `files list/read/remove/mkdir` | Uses the REPL and can interrupt the app |
| `control --action stop` | Explicitly requests the app to stop |

Do not read progress files between liveness screenshots. A REPL-backed read can trigger the app's `finally` cleanup, including LCD deinitialization; a later `DISPLAY_NOT_READY` is not by itself evidence of a firmware crash. Read progress after stop, or use a bounded foreground execution for live stdout. New CLI sessions cannot reattach to a prior run's stdout, and session-local `runtime.scriptRunning=false` does not prove global idleness.

## Evidence that matches the requested behavior

- A start acknowledgement, unchanged screenshot or session-local runtime field is insufficient to prove a continuous app is working. Use a visible increasing frame counter or another observable application state across passive captures.
- Separate drawing checks, a real model inference on a static fixture, live camera detections and no-detection scenes. A fixed overlay or a face visible in a camera image does not establish a detector hit. Record actual boxes/scores for a positive inference and label fixture screenshots.
- Base display layout on actual screen dimensions. Draw status text at native display resolution when preview scaling makes it unreadable.
- Check API return types and value semantics before using statistics in drawing or serialization. Do not assume statistics are integer counts or discard fractional values when serializing them; convert drawing coordinates explicitly to integers.
- Retain relevant versions, commands, screenshot metadata and errors. State which requested outcomes passed and which remain unverified. Test resource handoff with an explicit stop and a successful subsequent application initialization rather than inferring it from a fresh connection's state.
