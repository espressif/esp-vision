# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Real inference + LCD + bounded WiFi MJPEG. See README-ai-lcd-stress.md."""

import binascii
import errno
import gc
import hashlib
import json
import os
import socket
import time

import display
import esp32
import image
import network
import sensor


DEFAULTS = {
    "backend": "espdet",
    "model_path": "/sdcard/models/espdet/pico/face/espdet_pico_224_224_face.espdl",
    "model_sha256": None,
    "score": 0.5,
    "nms": 0.7,
    "duration_s": 1800,
    "port": 8080,
    "framesize": "QVGA",
    "frame_interval_ms": 100,
    "jpeg_quality": 60,
    "max_jpeg_bytes": 262144,
    "max_canvas_bytes": 2097152,
    "max_clients": 3,
    "request_timeout_ms": 2000,
    "send_timeout_ms": 2000,
    "http_budget_ms": 5,
    "latency_samples": 64,
    "gc_every": 10,
    "warmup_frames": 10,
    "halt_file": None,
    "result_path": None,
    "ssid": None,
    "password": None,
    "wifi_timeout_s": 20,
}
STOP_REQUESTED = False
LAST_RESULT = None
WOULD_BLOCK = (getattr(errno, "EAGAIN", 11), getattr(errno, "EWOULDBLOCK", 11))


def request_stop():
    global STOP_REQUESTED
    STOP_REQUESTED = True


def digest(data):
    return binascii.hexlify(hashlib.sha256(data).digest()).decode()


def model_digest(path):
    sha = hashlib.sha256()
    with open(path, "rb") as source:
        while True:
            block = source.read(4096)
            if not block:
                break
            sha.update(block)
    return binascii.hexlify(sha.digest()).decode()


def configuration(overrides=None):
    cfg = dict(DEFAULTS)
    if overrides is None:
        try:
            import stress_config
        except ImportError:
            overrides = {}
        else:
            overrides = stress_config.CONFIG
    for key in overrides:
        if key not in cfg:
            raise ValueError("Unknown stress configuration key: " + key)
    cfg.update(overrides)
    if cfg["backend"] not in ("espdet", "tflite_person"):
        raise ValueError("backend must be espdet or tflite_person")
    if cfg["backend"] == "tflite_person" and "model_path" not in overrides:
        cfg["model_path"] = "/sdcard/models/tflite/person_detect/person_detect.tflite"
    limits = {
        "port": (1, 65535), "duration_s": (0, 86400), "frame_interval_ms": (0, 10000),
        "jpeg_quality": (1, 100), "max_jpeg_bytes": (1024, 1048576),
        "max_canvas_bytes": (8192, 4194304), "max_clients": (1, 4),
        "request_timeout_ms": (100, 30000), "send_timeout_ms": (100, 30000),
        "http_budget_ms": (1, 20), "latency_samples": (8, 256),
        "gc_every": (1, 1000), "warmup_frames": (1, 10000), "wifi_timeout_s": (1, 120),
    }
    for key, bounds in limits.items():
        value = cfg[key]
        if not isinstance(value, int) or not bounds[0] <= value <= bounds[1]:
            raise ValueError("Invalid stress configuration: " + key)
    if not 0 <= cfg["score"] <= 1 or not 0 <= cfg["nms"] <= 1:
        raise ValueError("score and nms must be within 0..1")
    if cfg["result_path"] is not None and (not cfg["result_path"].endswith(".json") or cfg["result_path"] == cfg["model_path"]):
        raise ValueError("result_path must be a separate .json output file")
    return cfg


class Timing:
    """Lifetime mean/max, P95 of a fixed-size recent window, in milliseconds."""

    def __init__(self, size):
        self.values = [0] * size
        self.count = self.total = self.maximum = 0

    def add(self, milliseconds):
        self.values[self.count % len(self.values)] = milliseconds
        self.count += 1
        self.total += milliseconds
        self.maximum = max(self.maximum, milliseconds)

    def report(self):
        size = min(self.count, len(self.values))
        values = sorted(self.values[:size])
        return {"count": self.count, "mean_ms": self.total / max(1, self.count),
                "max_ms": self.maximum, "p95_ms": values[(size * 95 + 99) // 100 - 1] if size else 0,
                "window_samples": size}


class Inference:
    def __init__(self, cfg):
        self.backend = cfg["backend"]
        self.score = cfg["score"]
        if self.backend == "espdet":
            import espdl
            self.model = espdl.ESPDet(cfg["model_path"], score=self.score, nms=cfg["nms"])
        else:
            import tflite
            self.model = tflite.Model(cfg["model_path"])
            shape = self.model.input_shape[0]
            if len(shape) != 4 or shape[0] != 1 or shape[3] != 1:
                self.model.deinit()
                raise ValueError("tflite_person requires a NHWC grayscale person-detection model")
            self.height, self.width = shape[1:3]

    def detect(self, frame):
        if self.backend == "espdet":
            return self.model.detect(frame)
        side = min(frame.width(), frame.height())
        x, y = (frame.width() - side) // 2, (frame.height() - side) // 2
        small = frame.copy(roi=(x, y, side, side), x_scale=self.width / side,
                           y_scale=self.height / side, hint=image.BILINEAR, copy=True)
        small = small.to_grayscale(copy=False)
        pixels = small.bytearray()

        def fill(buffer, shape, dtype):
            if dtype not in (ord("b"), ord("B"), "b", "B"):
                raise ValueError("person model input must be int8 or uint8")
            signed = dtype in (ord("b"), "b")
            for i in range(shape[1] * shape[2] * shape[3]):
                buffer[i] = pixels[i] ^ 0x80 if signed else pixels[i]

        output = self.model.predict([fill])[0]
        try:
            output = output.reshape((2,))
        except AttributeError:
            pass
        scale, zero = self.model.output_scale[0], self.model.output_zero_point[0]
        absent, person = (float(output[0]) - zero) * scale, (float(output[1]) - zero) * scale
        # This is a classifier's input ROI, not a localized person bounding box.
        return [(x, y, side, side, person, 1)] if person >= absent and person >= self.score else []

    def deinit(self):
        self.model.deinit()


INDEX = b"""<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP-VISION AI LCD stress</title><style>body{background:#111;color:#eee;font:16px sans-serif}
img{max-width:95vw;max-height:75vh}pre{white-space:pre-wrap}</style>
<h2>Camera + real AI + LCD + WiFi</h2><img src=/stream.mjpg><p>
<button onclick="fetch('/control?freeze=1')">Freeze</button>
<button onclick="fetch('/control?freeze=0')">Resume</button>
<button onclick="fetch('/control?stop=1')">Stop and clean up</button>
<a href=/snapshot.jpg>Snapshot</a></p><pre id=m></pre><script>
setInterval(()=>fetch('/health.json').then(r=>r.json()).then(x=>m.textContent=JSON.stringify(x,null,2)),1000)
</script>"""


class Http:
    """Nonblocking, bounded clients; one retained frame per client, no frame queue."""

    def __init__(self, app):
        self.app = app
        self.clients = []
        self.counts = {key: 0 for key in (
            "accepted", "rejected", "closed", "close_errors", "request_timeout", "send_timeout", "read_errors",
            "write_errors", "invalid_requests", "snapshot_requests", "stream_requests", "metrics_requests",
            "control_requests", "raw_requests", "stream_rejected", "stream_frames_queued", "stream_frames_sent", "stream_skipped", "bytes_sent")}
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind(("0.0.0.0", app.cfg["port"]))
            self.server.listen(app.cfg["max_clients"])
            self.server.setblocking(False)
        except BaseException:
            self.server.close()
            raise

    def close_client(self, client):
        try:
            client["socket"].close()
        except OSError:
            self.counts["close_errors"] += 1
        finally:
            if client in self.clients:
                self.clients.remove(client)
                self.counts["closed"] += 1

    def close(self):
        for client in self.clients[:]:
            self.close_client(client)
        self.server.close()

    def queue(self, client, parts, frame=None):
        client["parts"] = [memoryview(part) for part in parts]
        client["part"] = client["offset"] = 0
        client["frame"] = frame  # Pins the JPEG Image backing bytearray()/memoryview().
        client["send_deadline"] = time.ticks_add(time.ticks_ms(), self.app.cfg["send_timeout_ms"])

    def response(self, client, code, content_type, body, extra=b"", frame=None):
        headers = ("HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n" % (code, content_type, len(body))).encode()
        self.queue(client, [headers + extra + b"\r\n", body], frame)

    def json_response(self, client, value, code="200 OK"):
        self.response(client, code, "application/json", json.dumps(value).encode())

    def frame_headers(self, frame):
        return ("X-Frame-Id: %d\r\nX-Canvas-SHA256: %s\r\nX-JPEG-SHA256: %s\r\n"
                "X-Canvas-Width: %d\r\nX-Canvas-Height: %d\r\n" % (
                    frame["frame_id"], frame["canvas_sha256"], frame["jpeg_sha256"],
                    frame["width"], frame["height"])).encode()

    def stream_frame(self, client):
        frame = self.app.latest
        if frame is None or frame["frame_id"] == client["last_id"]:
            return
        if client["last_id"]:
            self.counts["stream_skipped"] += max(0, frame["frame_id"] - client["last_id"] - 1)
        client["last_id"] = frame["frame_id"]
        headers = (b"--espvision\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n" % len(frame["jpeg"]))
        self.queue(client, [headers + self.frame_headers(frame) + b"\r\n", frame["jpeg"], b"\r\n"], frame)
        self.counts["stream_frames_queued"] += 1

    def route(self, client):
        line = bytes(client["request"]).split(b"\r\n", 1)[0].split()
        client["request"] = None
        if len(line) != 3 or line[0] != b"GET":
            self.counts["invalid_requests"] += 1
            self.response(client, "400 Bad Request", "text/plain", b"GET required")
            return
        path = line[1]
        frame = self.app.latest
        if path == b"/":
            self.response(client, "200 OK", "text/html; charset=utf-8", INDEX)
        elif path in (b"/health.json", b"/metrics"):
            self.counts["metrics_requests"] += 1
            self.json_response(client, self.app.health())
        elif path in (b"/snapshot.jpg", b"/frame.rgb565"):
            if frame is None:
                self.json_response(client, {"error": "FRAME_NOT_READY"}, "503 Service Unavailable")
            elif path == b"/snapshot.jpg":
                self.counts["snapshot_requests"] += 1
                self.response(client, "200 OK", "image/jpeg", frame["jpeg"], self.frame_headers(frame), frame)
            elif not self.app.frozen:
                self.json_response(client, {"error": "FREEZE_FIRST"}, "409 Conflict")
            else:
                self.counts["raw_requests"] += 1
                client["raw"] = True
                self.response(client, "200 OK", "application/octet-stream", self.app.canvas.bytearray(), self.frame_headers(frame), frame)
        elif path == b"/stream.mjpg":
            # Reserve a slot for health/control even when streams remain open.
            if sum(1 for item in self.clients if item["stream"]) >= self.app.cfg["max_clients"] - 1:
                self.counts["stream_rejected"] += 1
                self.json_response(client, {"error": "STREAM_LIMIT"}, "503 Service Unavailable")
                return
            self.counts["stream_requests"] += 1
            client["stream"] = True
            self.queue(client, [b"HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=espvision\r\nCache-Control: no-store\r\n\r\n"])
        elif path in (b"/control?freeze=1", b"/control?freeze=0", b"/control?stop=1"):
            self.counts["control_requests"] += 1
            if path == b"/control?freeze=0" and any(item.get("raw") for item in self.clients):
                self.json_response(client, {"error": "RAW_TRANSFER_ACTIVE"}, "409 Conflict")
            else:
                if path == b"/control?stop=1":
                    request_stop()
                else:
                    self.app.frozen = path == b"/control?freeze=1"
                self.json_response(client, {"ok": True, "frozen": self.app.frozen, "stopping": STOP_REQUESTED,
                                            "frame_id": frame["frame_id"] if frame else None})
        else:
            self.response(client, "404 Not Found", "text/plain", b"Not found")

    def pump(self):
        deadline = time.ticks_add(time.ticks_ms(), self.app.cfg["http_budget_ms"])
        try:
            sock, _address = self.server.accept()
        except OSError as exc:
            if exc.args[0] not in WOULD_BLOCK:
                raise
        else:
            sock.setblocking(False)
            if len(self.clients) >= self.app.cfg["max_clients"]:
                sock.close()
                self.counts["rejected"] += 1
            else:
                self.counts["accepted"] += 1
                self.clients.append({"socket": sock, "request": bytearray(), "parts": [], "stream": False,
                                     "last_id": 0, "deadline": time.ticks_add(time.ticks_ms(), self.app.cfg["request_timeout_ms"])})
        for client in self.clients[:]:
            if time.ticks_diff(time.ticks_ms(), deadline) >= 0:
                break
            if client["request"] is not None:
                if time.ticks_diff(time.ticks_ms(), client["deadline"]) >= 0:
                    self.counts["request_timeout"] += 1
                    self.close_client(client)
                    continue
                try:
                    data = client["socket"].recv(256)
                    if not data:
                        self.close_client(client)
                        continue
                    client["request"].extend(data)
                    if len(client["request"]) > 1024:
                        self.counts["invalid_requests"] += 1
                        self.close_client(client)
                        continue
                    if b"\r\n\r\n" in client["request"]:
                        self.route(client)
                except OSError as exc:
                    if exc.args[0] not in WOULD_BLOCK:
                        self.counts["read_errors"] += 1
                        self.close_client(client)
                continue
            if not client["parts"]:
                if client["stream"]:
                    self.stream_frame(client)
                else:
                    self.close_client(client)
                    continue
            if not client["parts"]:
                continue
            if time.ticks_diff(time.ticks_ms(), client["send_deadline"]) >= 0:
                self.counts["send_timeout"] += 1
                self.close_client(client)
                continue
            try:
                # Fixed chunk and operation limits also bound work on coarse clocks.
                for _ in range(8):
                    if time.ticks_diff(time.ticks_ms(), deadline) >= 0:
                        break
                    part = client["parts"][client["part"]]
                    sent = client["socket"].send(part[client["offset"]:client["offset"] + 2048])
                    if not sent:
                        raise OSError(errno.EPIPE)
                    self.counts["bytes_sent"] += sent
                    client["offset"] += sent
                    if client["offset"] == len(part):
                        client["part"] += 1
                        client["offset"] = 0
                        if client["part"] == len(client["parts"]):
                            if client["stream"] and client.get("frame") is not None:
                                self.counts["stream_frames_sent"] += 1
                            client["parts"] = []
                            client["frame"] = None
                            break
            except OSError as exc:
                if exc.args[0] not in WOULD_BLOCK:
                    self.counts["write_errors"] += 1
                    self.close_client(client)


class Stress:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lcd = self.canvas = self.inference = self.http = self.wlan = self.latest = None
        self.wifi_before = None
        self.wifi_connected_here = False
        self.sensor_started = self.frozen = False
        self.start_ms = time.ticks_ms()
        self.stage = "startup"
        self.status = "starting"
        self.model_sha = None
        self.stats = {"frames": 0, "inferences": 0, "detections": 0, "errors": 0, "cleanup_errors": 0,
                      "gc_runs": 0, "heap_free": 0, "min_heap": None,
                      "steady_heap": None, "steady_min_heap": None}
        self.timings = {key: Timing(cfg["latency_samples"]) for key in ("capture", "inference", "compose", "jpeg", "lcd", "pipeline")}
        self.cleanup = {}
        self.final_frame = None

    def connect_wifi(self):
        self.wlan = network.WLAN(network.STA_IF)
        self.wifi_before = (self.wlan.active(), self.wlan.isconnected())
        if not self.wifi_before[1]:
            if not self.cfg["ssid"]:
                raise OSError("No existing STA connection; provide WiFi in external stress_config.CONFIG")
            self.wlan.active(True)
            self.wifi_connected_here = True
            self.wlan.connect(self.cfg["ssid"], self.cfg["password"] or "")
            deadline = time.ticks_add(time.ticks_ms(), self.cfg["wifi_timeout_s"] * 1000)
            while not self.wlan.isconnected():
                if STOP_REQUESTED or time.ticks_diff(time.ticks_ms(), deadline) >= 0:
                    raise OSError("WiFi connection did not become ready")
                time.sleep_ms(100)
        self.ip = self.wlan.ifconfig()[0]

    def heap(self, collected=False):
        free = gc.mem_free()
        self.stats["heap_free"] = free
        current = self.stats["min_heap"]
        self.stats["min_heap"] = free if current is None else min(free, current)
        if self.stats["frames"] >= self.cfg["warmup_frames"]:
            current = self.stats["steady_min_heap"]
            self.stats["steady_min_heap"] = free if current is None else min(free, current)
            if collected:
                self.stats["steady_heap"] = free
        if collected:
            regions = esp32.idf_heap_info(esp32.HEAP_DATA)
            self.stats["idf_heap_free"] = sum(region[1] for region in regions)
            self.stats["idf_largest_block"] = max([region[2] for region in regions] or [0])
            self.stats["idf_region_min_free_sum"] = sum(region[3] for region in regions)

    def health(self):
        frame = self.latest or self.final_frame
        result = {"status": self.status, "stage": self.stage, "backend": self.cfg["backend"], "model_sha256": self.model_sha,
                  "elapsed_ms": time.ticks_diff(time.ticks_ms(), self.start_ms), "frozen": self.frozen,
                  "frame_id": frame["frame_id"] if frame else None, "stats": dict(self.stats),
                  "latency": {key: value.report() for key, value in self.timings.items()},
                  "http": dict(self.http.counts) if self.http else {}, "cleanup": dict(self.cleanup)}
        if frame:
            result.update({key: frame[key] for key in ("width", "height", "canvas_sha256", "jpeg_sha256", "jpeg_bytes")})
        return result

    def setup(self):
        self.connect_wifi()
        self.stage = "model"
        self.model_sha = model_digest(self.cfg["model_path"])
        if self.cfg["model_sha256"] and self.model_sha != self.cfg["model_sha256"]:
            raise ValueError("Model SHA256 mismatch")
        self.inference = Inference(self.cfg)
        self.stage = "display"
        self.lcd = display.Display()
        width, height = self.lcd.width(), self.lcd.height()
        if width < 64 or height < 64 or width * height * 2 > self.cfg["max_canvas_bytes"]:
            raise ValueError("LCD dimensions exceed the configured canvas bound")
        self.canvas = image.Image(width, height, image.RGB565)
        self.stage = "sensor"
        self.sensor_started = True
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(getattr(sensor, self.cfg["framesize"]))
        sensor.skip_frames(time=500)
        self.http = Http(self)
        self.status = "running"

    def frame(self):
        begin = mark = time.ticks_ms()
        self.stage = "capture"
        frame = sensor.snapshot()
        now = time.ticks_ms(); self.timings["capture"].add(time.ticks_diff(now, mark)); mark = now
        self.stage = "inference"
        detections = self.inference.detect(frame)
        self.stats["inferences"] += 1  # Only after real detect()/predict() returned.
        self.stats["detections"] += len(detections)
        now = time.ticks_ms(); self.timings["inference"].add(time.ticks_diff(now, mark)); mark = now
        self.stage = "compose"
        for x, y, width, height, score, category in detections[:20]:
            frame.draw_rectangle(int(x), int(y), int(width), int(height), color=(255, 0, 0), thickness=2)
            frame.draw_string(int(x), max(0, int(y) - 12), "%.2f:%d" % (score, category), color=(255, 255, 0))
        canvas = self.canvas
        width, height = canvas.width(), canvas.height()
        canvas.draw_rectangle(0, 0, width, height, color=(0, 0, 0), fill=True)
        scale = min(width / frame.width(), (height - 42) / frame.height())
        x, y = (width - int(frame.width() * scale)) // 2, 42 + (height - 42 - int(frame.height() * scale)) // 2
        canvas.draw_image(frame, x, y, x_scale=scale, y_scale=scale)
        frame_id = self.stats["frames"] + 1
        canvas.draw_string(2, 2, "F%d AI%d D%d" % (frame_id, self.stats["inferences"], len(detections)), color=(255, 255, 255))
        canvas.draw_string(2, 16, "AI %dms heap %dK" % (self.timings["inference"].values[(self.timings["inference"].count - 1) % len(self.timings["inference"].values)], gc.mem_free() // 1024), color=(255, 255, 255))
        canvas.draw_string(2, 30, "errors %d" % self.stats["errors"], color=(255, 255, 255))
        raw_sha = digest(canvas.bytearray())
        now = time.ticks_ms(); self.timings["compose"].add(time.ticks_diff(now, mark)); mark = now
        self.stage = "jpeg"
        jpeg_image = canvas.to_jpeg(quality=self.cfg["jpeg_quality"], copy=True)
        data = memoryview(jpeg_image.bytearray())
        if len(data) > self.cfg["max_jpeg_bytes"]:
            raise ValueError("JPEG exceeds max_jpeg_bytes")
        jpeg_sha = digest(data)
        now = time.ticks_ms(); self.timings["jpeg"].add(time.ticks_diff(now, mark)); mark = now
        self.stage = "lcd"
        self.lcd.write(canvas, x=0, y=0, x_scale=1.0, y_scale=1.0, fit=False)
        if canvas.format() != image.RGB565 or digest(canvas.bytearray()) != raw_sha:
            raise RuntimeError("JPEG/display changed the shared RGB565 canvas")
        self.stats["frames"] = frame_id
        self.latest = {"frame_id": frame_id, "width": width, "height": height, "canvas_sha256": raw_sha,
                       "jpeg_sha256": jpeg_sha, "jpeg_bytes": len(data), "jpeg_image": jpeg_image, "jpeg": data}
        now = time.ticks_ms(); self.timings["lcd"].add(time.ticks_diff(now, mark))
        self.timings["pipeline"].add(time.ticks_diff(now, begin))
        self.heap()
        if frame_id % self.cfg["gc_every"] == 0:
            gc.collect(); self.stats["gc_runs"] += 1; self.heap(True)
        if frame_id == 1:
            print("STRESS_READY " + json.dumps({"ip": self.ip, "port": self.cfg["port"], "backend": self.cfg["backend"],
                                                "frame_id": frame_id, "width": width, "height": height, "model_sha256": self.model_sha}))

    def finish(self):
        if self.latest:
            self.final_frame = {key: self.latest[key] for key in (
                "frame_id", "width", "height", "canvas_sha256", "jpeg_sha256", "jpeg_bytes")}
        actions = [("http", self.http.close if self.http else None),
                   ("model", self.inference.deinit if self.inference else None),
                   ("sensor", sensor.shutdown if self.sensor_started else None),
                   ("display", self.lcd.deinit if self.lcd else None)]
        for name, action in actions:
            try:
                if action:
                    action()
                self.cleanup[name] = True
            except Exception:
                self.cleanup[name] = False
                self.stats["cleanup_errors"] += 1
        try:
            if self.wifi_connected_here:
                self.wlan.disconnect()
            if self.wifi_before is not None and not self.wifi_before[0]:
                self.wlan.active(False)
            self.cleanup["wifi"] = True
        except Exception:
            self.cleanup["wifi"] = False
            self.stats["cleanup_errors"] += 1
        if self.stats["cleanup_errors"]:
            self.status = "error"
        self.latest = self.canvas = self.lcd = self.inference = None
        gc.collect()
        self.stats["final_heap_free"] = gc.mem_free()


def run(config=None):
    global STOP_REQUESTED, LAST_RESULT
    STOP_REQUESTED = False
    app = Stress(configuration(config))
    reason = "duration"
    try:
        app.setup()
        next_frame = time.ticks_ms()
        next_halt_check = next_frame
        while True:
            now = time.ticks_ms()
            if STOP_REQUESTED:
                reason = "stop"
                break
            if app.cfg["duration_s"] and time.ticks_diff(now, app.start_ms) >= app.cfg["duration_s"] * 1000:
                break
            if app.cfg["halt_file"] and time.ticks_diff(now, next_halt_check) >= 0:
                next_halt_check = time.ticks_add(now, 1000)
                try:
                    os.stat(app.cfg["halt_file"])
                except OSError as exc:
                    if exc.args[0] != errno.ENOENT:
                        raise
                else:
                    reason = "halt_file"
                    break
            app.stage = "http"
            app.http.pump()
            if STOP_REQUESTED:
                continue
            if (not app.frozen or app.latest is None) and time.ticks_diff(now, next_frame) >= 0:
                app.frame()
                next_frame = time.ticks_add(time.ticks_ms(), app.cfg["frame_interval_ms"])
            time.sleep_ms(1)
        # Allow a stop HTTP response to drain; never wait indefinitely for clients.
        until = time.ticks_add(time.ticks_ms(), 200)
        while time.ticks_diff(time.ticks_ms(), until) < 0:
            app.http.pump()
            time.sleep_ms(1)
        app.status = "stopped"
    except KeyboardInterrupt:
        app.status = "stopped"
        reason = "interrupt"
    except BaseException:
        app.stats["errors"] += 1
        app.status = "error"
        reason = app.stage
        raise
    finally:
        app.finish()
        LAST_RESULT = app.health()
        LAST_RESULT["stop_reason"] = reason
        if app.cfg["result_path"]:
            with open(app.cfg["result_path"], "w") as result:
                json.dump(LAST_RESULT, result)
        print("STRESS_FINAL " + json.dumps(LAST_RESULT))
    return LAST_RESULT


if __name__ == "__main__":
    run()
