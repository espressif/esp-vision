# Camera, real inference, LCD and WiFi stress example

`wifi_ai_lcd_stress.py` combines the camera, a real ESP-DL ESPDet detector (or an explicitly selected TFLite person classifier), LCD rendering and a nonblocking HTTP server. The default duration is 1,800 seconds. It derives its camera/inference APIs from `espdet_pico.py` and `person_detection.py`, and its HTTP JPEG format from `wifi_mjpeg_stream.py`. No inference is simulated and there is no automatic backend fallback.

The ESP-VISION Knowledge MCP lists `sensor`, `image`, `display`, `espdl` and `tflite` for both ESP32-S31 and ESP32-P4. A runtime being available does not establish that a particular `.espdl` export targets that chip: the repository Pico face JSON sidecar does not specify a target. First load the selected model and complete a real inference on each board. Record `backend`, `model_sha256` and increasing `stats.inferences`; use the explicitly selected TFLite backend if the ESP-DL model is incompatible. Model loading or inference failure stops the example with an error and releases resources.

## Run

Copy the script and the selected model to accessible board storage. Keep model metadata beside the binary when copying the repository model directory. A host-managed temporary RAM filesystem also works; no filesystem is formatted by this example. Default model paths are `/sdcard/models/espdet/pico/face/espdet_pico_224_224_face.espdl` for ESPDet and `/sdcard/models/tflite/person_detect/person_detect.tflite` for TFLite. ESPDet draws detection boxes; the TFLite classifier draws its centered input region when a person is detected, which is a classification ROI rather than a localized bounding box.

Put an external `stress_config.py` on the Python import path, containing a `CONFIG` dictionary. The example file contains no credentials. It first reuses an existing STA connection, even when optional credentials are supplied. For example:

```python
CONFIG = {
    "backend": "espdet",
    "model_path": "/sdcard/models/espdet/pico/face/espdet_pico_224_224_face.espdl",
    "duration_s": 1800,
    "port": 8080,
    "result_path": "/stress-result.json",
    "halt_file": "/stress.stop",
}
```

Only provide `ssid` and `password` in that external configuration if no STA link is already connected. The example does not change an existing connected link. If it creates a link, cleanup disconnects it and restores the original STA active/inactive state. It cannot recover an unknown prior saved WiFi password; do not use the optional connect path when credential-store restoration is required. Credentials are never included in health output. This plain HTTP control interface is intended for a trusted test LAN.

Execute the script, or import it and pass configuration directly without creating a configuration file:

```python
import wifi_ai_lcd_stress
wifi_ai_lcd_stress.run({
    "model_path": "/evtest/face.espdl",
    "duration_s": 1800,
    "result_path": "/evtest/result.json",
})
```

`STRESS_READY` is printed only after the first successful inference, JPEG encoding and LCD submission. Open `http://<board-ip>:8080/`. For host acceptance, keep the public Device Core execution session open when Python output is needed. A detached run's stdout cannot be reattached by a new session; HTTP health and the optional final JSON provide independent observations.

## One composed image, with verifiable frame identity

Each iteration takes exactly one application camera snapshot and runs actual inference on it. It draws results into that image, then composes a reusable RGB565 canvas at the LCD's physical `width()` and `height()`. The camera view is scaled proportionally into the space below a frame-number/inference/heap overlay, with explicit black margins. Thus S31's 800×480 and P4's different panel dimensions use their own full-panel canvas without silently clipping the HTTP image.

The canvas is encoded with `to_jpeg(copy=True)` and submitted with `lcd.write(canvas, fit=False, x_scale=1, y_scale=1)`. It verifies that encoding/submission did not change the RGB565 canvas. HTTP never calls `sensor.snapshot()` and never applies another crop or resize. A published frame carries its frame ID, raw canvas SHA-256, JPEG SHA-256 and physical dimensions. Hashing reads the existing bytearray; it does not create a full-size raw copy.

JPEG is lossy. HTTP and the debug screen-capture encoder may use different JPEG encoders, so their JPEG bytes or decoded pixels are not promised to be identical. They represent the same composed source canvas. Live HTTP clients can lag behind the current LCD because frames are dropped rather than queued. To compare a specific frame:

1. Request `/control?freeze=1`, then read `/health.json` to record `frame_id`, dimensions and both hashes. Freezing stops new snapshots/inferences while the server remains responsive.
2. Download `/snapshot.jpg`; verify its `X-Frame-Id`, `X-Canvas-SHA256`, `X-JPEG-SHA256`, dimensions and JPEG hash.
3. Download `/frame.rgb565` and verify its byte length (`width * height * 2`) and SHA-256. This endpoint requires freeze; resume returns HTTP 409 while a raw transfer remains active.
4. Capture the actual LCD shadow through the public Core `capture({ source: 'screen' })` operation while still frozen. Compare panel dimensions, the visible frame number, boxes and overlay positions, allowing JPEG loss. This screenshot is independent evidence that the expected canvas reached the display; the application's hash alone is not a readback of the panel electronics.
5. Request `/control?freeze=0` to resume. End freeze checks before measuring inference progress or full-run throughput; elapsed duration includes frozen time.

## HTTP and stop interfaces

| Endpoint | Behavior |
| --- | --- |
| `/` | Browser MJPEG view, metrics and freeze/resume/stop buttons |
| `/snapshot.jpg` | Last successfully published LCD canvas as JPEG; no new camera acquisition |
| `/stream.mjpg` | Multipart JPEG; each part includes frame ID and canvas/JPEG hashes |
| `/health.json`, `/metrics` | JSON counters, heap observations, model identity and latency summaries |
| `/frame.rgb565` | Frozen source canvas bytes, with the same frame identity headers |
| `/control?freeze=1`, `/control?freeze=0` | Freeze/resume capture and inference |
| `/control?stop=1` | Request cooperative termination and cleanup |

Duration expiry, `request_stop()`, HTTP stop, the optional halt file, and Ctrl-C all pass through `finally`. The halt file is checked once per second and is never deleted by the example. Ensure it does not already exist before starting. Native capture/inference calls are synchronous: termination is checked between operations and cannot preempt a blocked native driver. Cleanup closes every socket, deinitializes the model, shuts down the sensor, deinitializes the display and restores the STA link/active state described above. The optional result file records cleanup outcomes, the stop reason and final counters; no application startup file or model is overwritten.

Each client has a maximum 1,024-byte request and one in-flight frame. The server uses nonblocking sockets, a fixed client limit, fixed-size send chunks, per-pump operation/time budgets, and absolute request/frame-send deadlines. At most `max_clients - 1` connections may stream, reserving capacity for health/control; with `max_clients=1`, only finite responses are available. A slow client is disconnected after its deadline; its timeout or write error increments HTTP counters rather than the application's fatal error count. Multipart clients skip intermediate frames. There is no growing queue. One latest JPEG, at most one old JPEG per client, and a transient encoding result can coexist; the reusable canvas and display shadow also consume memory. `max_jpeg_bytes` checks encoded output size, rather than limiting the encoder's internal transient allocation.

## Configuration

Unspecified fields use these defaults; unknown fields and out-of-range numeric limits are rejected.

| Field | Default | Meaning |
| --- | --- | --- |
| `backend` | `"espdet"` | `espdet` or `tflite_person`; explicit choice, no fallback |
| `model_path` | See above | Readable `.espdl` or `.tflite` file |
| `model_sha256` | `None` | Optional expected model hash, checked before loading |
| `score`, `nms` | `0.5`, `0.7` | Score threshold; ESPDet NMS threshold |
| `duration_s` | `1800` | Wall-clock duration including setup/freeze; `0` runs until stopped, maximum 86400 |
| `port` | `8080` | HTTP port |
| `framesize` | `"QVGA"` | Public `sensor` framesize constant name; capture format is RGB565 |
| `frame_interval_ms` | `100` | Minimum pause after a completed frame, not a target FPS |
| `jpeg_quality` | `60` | HTTP canvas JPEG quality, 1–100 |
| `max_jpeg_bytes` | `262144` | Reject a published JPEG above this size |
| `max_canvas_bytes` | `2097152` | Maximum physical RGB565 canvas size |
| `max_clients` | `3` | Simultaneous clients, maximum 4; streams reserve one slot for control/health |
| `request_timeout_ms` | `2000` | Absolute request-header deadline |
| `send_timeout_ms` | `2000` | Absolute deadline for one queued response/MJPEG frame |
| `http_budget_ms` | `5` | Cooperative budget per HTTP pump; sends use at most 8×2048-byte chunks per client |
| `latency_samples` | `64` | Fixed rolling P95 sample count per pipeline stage |
| `gc_every`, `warmup_frames` | `10`, `10` | Explicit GC cadence and start of steady heap observations |
| `halt_file`, `result_path` | `None`, `None` | Optional existing-file stop signal and final JSON output path |
| `ssid`, `password` | `None`, `None` | External-only WiFi credentials, used only if initially disconnected |
| `wifi_timeout_s` | `20` | WiFi connection deadline |

`stats.inferences` increases only after a successful native `detect()`/`predict()`, while `stats.frames` increases only after JPEG encoding and LCD submission. Zero detected objects does not mean zero inference. Stage errors stop the run rather than pretending inference succeeded. `stats.min_heap` is the smallest sampled `gc.mem_free()` value; `steady_heap` is the latest post-GC value after warmup, and `steady_min_heap` is the minimum sampled value after warmup. `final_heap_free` is observed after cleanup. With automatic split heaps, `gc.mem_free()` includes the largest native block available for future GC expansion; it is not a guarantee that a native DMA/framebuffer allocation will succeed. Native IDF free memory and largest free block are sampled after GC; `idf_region_min_free_sum` sums each region's historical low-water mark, rather than claiming a simultaneous global minimum. Latency reports contain lifetime mean/max and the rolling-window P95 in milliseconds; they are not full-run P95 values.

Model files staged in a RAM filesystem remain resident in addition to the inference runtime's model buffers. Prefer an existing mounted storage volume when available, and include RAM staging in the native-memory budget when using it for testing. S31's SC101IOT camera captures 1280×720 raw input even when the application requests QVGA output; the board uses one queued frame plus the controller's independent backup frame to leave room for AI, the full LCD canvas and JPEG encoding. This can drop camera frames while the application is busy, so record actual completed capture/inference throughput.

Record each board's model hash, actual inference count and time series of heap/latency/HTTP counters for the 30-minute acceptance run. Exercise browser, snapshot, MJPEG, disconnecting and deliberately slow clients. HTTP request/send timeout counters may rise under that deliberate load; fatal `stats.errors`, cleanup failures, stalled inference outside an intentional freeze, heap exhaustion or incompatible models are separate failures. Hardware acceptance results belong with their exact firmware/source provenance; this example does not claim unperformed board tests.
