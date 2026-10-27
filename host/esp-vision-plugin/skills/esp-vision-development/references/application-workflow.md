# Installation and application verification

Use this reference for installation and serial-adapter troubleshooting, continuous applications, or live camera/LCD verification. Resolve the installed plugin root and the user-selected device first. Read actual device storage before choosing a new application path; do not assume `/flash` or an SD card exists. Upload dependencies and finish directory operations before starting the app.

## Installation and host context

A matching version string does not prove that an installed directory matches the immutable published npm package. An earlier development snapshot can use the same version while lacking later manifests or assets. Compare the installed manifest and referenced files with the official package before attributing an icon or missing-file symptom to the published release.

For an incomplete installer-owned development copy, preserve the full runtime directory and native plugin cache, including local changes. Use the host's native commands to uninstall only the ESP-VISION plugin and remove its marketplace registration, retaining user data where supported. After confirming directory ownership and preserving the backup, move the old installer-owned `plugins/<version>` directory aside so the installer can create a clean copy; then reinstall the pinned official npm release for that host. Restart the host and verify its native plugin/marketplace lists and installed resources. Do not patch assets in place, force an overwrite, edit host configuration directly, or discard an unrecognized directory. Do not claim a repair option exists unless the selected published installer actually provides it.

WorkBuddy currently exports a Skill ZIP, separate Knowledge MCP configuration and `INSTALL.md`. `manual-install-required` means those artifacts are prepared; it does not mean WorkBuddy has imported or activated them. Complete both documented UI steps and verify the Knowledge connection in a new conversation. Knowledge MCP supplies documentation; local device operations intentionally use the bundled CLI rather than a Device MCP server. A separate maintainer build now prepares an Expert ZIP with an Agent, Skill and Knowledge MCP dependency; that artifact requires the Expert publication workflow and its own target-host activation checks. Do not treat it as the legacy npm installer's output or infer that either format requires a Device MCP.

## Hardware API and capture context

### Troubleshooting Q&A

**Q: On Windows, `files list --path /models` returns `INVALID_PATH`. Is `/models` invalid on the device?**

A: Preserve the original error and determine which arguments reached the CLI before changing the device path. Git Bash/MSYS can rewrite a Unix-looking argument such as `/models` into a Windows host path when launching native Node. This is a possible shell-side cause, not a confirmed explanation for every `INVALID_PATH`. See the [MSYS2 argument conversion documentation](https://www.msys2.org/docs/filesystem-paths/). Quoting `/models` alone does not reliably prevent this conversion. Device paths belong to device storage; CLI scripts, upload sources and download destinations belong to the host filesystem. Never remove a drive letter or a Git installation prefix from an already-rewritten argument to guess the intended device path.

Prefer PowerShell on Windows. Set CLI to the verified installed CLI file's Windows path and PORT to the explicitly selected endpoint. Only run a REPL-backed file listing when its possible interruption of a running program is intended. For the user's requested `/models` directory, the PowerShell command is:

```powershell
if (-not $CLI -or -not $PORT) { throw 'Set CLI to the verified installed file and PORT to the selected serial device' }
node "$CLI" files list --path /models --port "$PORT" --json
```

When Git Bash/MSYS must be used, disable argument conversion for that one command. First set CLI to the verified Windows path accepted by native Node (including its drive letter), not an MSYS mount path. Local source/output paths must likewise already be verified Windows paths because conversion is disabled. Do not change persistent shell settings:

```sh
MSYS2_ARG_CONV_EXCL='*' node "${CLI:?Set CLI to the verified installed bundle Windows path}" files list --path /models --port "${PORT:?Set PORT to the explicitly selected serial device}" --json
```

When shell rewriting remains uncertain, explain that this task-relevant diagnostic only prints received arguments and does not access a device; follow the host's required authorization flow. Run these two Git Bash/MSYS commands separately and compare whether `/models` arrives unchanged:

```sh
node -e 'console.log(JSON.stringify(process.argv.slice(1)))' -- /models
MSYS2_ARG_CONV_EXCL='*' node -e 'console.log(JSON.stringify(process.argv.slice(1)))' -- /models
```

Keep the shell/OS, verified CLI path, original command/error and argument output as evidence. If `/models` already reaches Node unchanged, investigate the actual path validation and device storage result rather than attributing the error to MSYS. Do not retry a device change or interrupt a running application merely to test this hypothesis.

**Q: On Windows, camera capture returns `SERIAL_ERROR`, but Python is installed. What should I check?**

A: A serial-adapter failure does not establish a camera or firmware defect. Check the Python command actually used by the CLI, its pyserial dependency and access to the selected port. In [GitHub issue #17](https://github.com/espressif/esp-vision/issues/17), a Qoder session reported Python 3.14.5 available through the Windows `py` launcher while the default `python` command failed; repeating the intended capture with `--python py` succeeded. This is a reported workaround for that environment, not proof that every serial error has the same cause. Preserve the original error. Read the installed CLI's `--help`, and use an explicitly verified interpreter through `--python` for the intended operation; do not change the selected device or reset it to diagnose a host dependency.

For a user-selected serial endpoint and verified installed CLI path, the PowerShell form is `node "$CLI" capture camera --port "$PORT" --output "$OUTPUT" --python py`. Set CLI, PORT and OUTPUT from verified installation, explicit user selection and the workspace output path; do not copy the issue's COM9 as a default. Camera capture may still fail for independent device or firmware reasons.

**Q: How should I check or install pyserial?**

A: Only perform dependency checks when relevant to the observed adapter error. Explain their purpose first. If `py` is the chosen interpreter, use `py --version` and `py -c "import serial; print(serial.__version__)"` as separate checks. Use the same interpreter for dependency installation, such as `py -m pip install pyserial`, through the host's required approval flow. For an explicit Python executable, use that executable with `-m pip`; an unrelated bare `pip` may install into another environment. Do not claim a dependency is missing based only on a generic `SERIAL_ERROR` or `ENUMERATION_UNAVAILABLE`.

**Q: The host blocks a diagnostic command. Is that a device failure?**

A: No. Distinguish a host policy/classifier rejection, filesystem/network/serial permission denial, missing executable/module, port ownership conflict and device-protocol error using the actual message. A classifier rejection alone does not establish a filesystem, sandbox or device-access restriction. Explain the minimal check's purpose and whether it accesses or changes the device, then use the host's native authorization mechanism for that specific step. Do not repeatedly retry or switch tools/interpreters to evade a denial. If authorization is unavailable, retain the error, provide a manual command and continue independent work. Local execution approval does not authorize flashing, reset, stopping an application or storage changes. Never ask users to disable security protections.

**Q: Discovery fails or the serial port is busy. Should I reset the board or take over the port?**

A: No. `ENUMERATION_UNAVAILABLE` means inventory sources failed, not that no device exists. Report the failed discovery and check relevant host prerequisites without probing or resetting the device. For a busy port, identify the owning session when possible and have its owner release it; do not force takeover. Ask the user to select an endpoint before device operations. A successful `--help` verifies CLI startup only, not discovery, connectivity or camera operation.

**Q: Should I send this problem through Knowledge MCP?**

A: No. Knowledge MCP provides read-only documentation queries. Keep evidence locally, search [GitHub Issues](https://github.com/espressif/esp-vision/issues), prepare the appropriate English issue draft and obtain user authorization before submission or attachment upload. Offer a manual draft if submission is unavailable. Follow [SECURITY.md](https://github.com/espressif/esp-vision/security/policy) for private vulnerability reports. A successful CLI capture in a Qoder session does not by itself prove role/Skill activation, Knowledge integration or full host acceptance.

### Camera and ISP acceptance

Before using board-specific APIs, query official knowledge for the actual board, sensor, chip revision and firmware release, then inspect the selected device. An advertised capture capability alone does not prove that older firmware supports capture while a continuous Python or native camera application is running. Verify that mode on the actual release; retain a live-capture failure and mark the outcome unverified. Use a stopped-application fallback only when it satisfies the user's request, with an explicit stop first.

`sensor.get_isp()` reads supported driver parameters; it is not a frame fence and does not identify the settings applied to a particular `snapshot()`. IPA controls the configured image-processing algorithms; toggling IPA does not establish the image sensor's own AE/AWB state. Check the matching API and active sensor/IPA configuration before drawing conclusions. Allow and observe settling when comparing images rather than treating immediate readback as per-frame proof.

These checks help distinguish installation state, API semantics and unsupported modes from defects. They do not dismiss a reproducible failure: retain the package and firmware identity, selected endpoint, minimal commands, expected/observed behavior and relevant output or captures, then use the main Skill's reporting workflow for confirmed official problems.

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
