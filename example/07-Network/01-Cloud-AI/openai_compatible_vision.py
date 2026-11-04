# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Press the user button to capture one camera frame and send it to an
# OpenAI-compatible vision API.
#
# Set API_KEY below, or leave it empty when API_URL points to an
# unauthenticated local proxy.

import binascii
import errno
import gc
import json
import network
import socket
import time
from machine import Pin

import sensor

SSID = "your-ssid"
PASSWORD = "your-wifi-password"

# ESP32_P4X_EYE user button is GPIO3. ESP32_S31_KORVO BOOT key is GPIO61.
BUTTON_PIN = 3
BUTTON_PRESSED_LEVEL = 0

API_URL = "https://api.openai.com/v1/chat/completions"
API_KEY = "your-api-key"
MODEL = "gpt-4o-mini"
SYSTEM_PROMPT = "You are an AI vision assistant."
PROMPT = "Describe this image and list the visible objects. Keep it short."

CONNECT_TIMEOUT_S = 15
CONNECT_RETRY = 3
JPEG_QUALITY = 35
FRAME_DELAY_MS = 20
MAX_TOKENS = 120
TLS_VERIFY = False


def connect_wifi():
    wlan = network.WLAN(network.STA_IF)

    for attempt in range(1, CONNECT_RETRY + 1):
        print("connecting:", SSID, "attempt:", attempt)

        deadline = time.ticks_add(time.ticks_ms(), CONNECT_TIMEOUT_S * 1000)
        connect_requested = False
        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            if wlan.isconnected():
                if wlan.config("ssid") != SSID:
                    raise OSError("another WiFi SSID is connected; configure the target with Device Core")
                print("wifi connected:", wlan.ifconfig()[0])
                return wlan

            try:
                # Do not disconnect a managed STA. Change saved WiFi with Device Core.
                if not wlan.active():
                    wlan.active(True)
                if not connect_requested and wlan.status() != network.STAT_CONNECTING:
                    wlan.connect(SSID, PASSWORD)
                    connect_requested = True
            except OSError as exc:
                # The background network service may still own a connection attempt.
                if not exc.args or exc.args[0] != errno.EBUSY:
                    print("connect failed:", exc)
                    time.sleep_ms(500)
                    break
            time.sleep_ms(200)
        else:
            print("connect timed out")

    raise OSError("wifi connect failed; check the target WiFi with Device Core")


class _HttpClient:
    def __init__(self, tls_verify=False):
        self.tls_verify = tls_verify

    def _parse_url(self, url):
        if url.startswith("https://"):
            scheme = "https"
            rest = url[8:]
            port = 443
        elif url.startswith("http://"):
            scheme = "http"
            rest = url[7:]
            port = 80
        else:
            raise ValueError("url must start with http:// or https://")

        path_start = rest.find("/")
        if path_start < 0:
            host_port = rest
            path = "/"
        else:
            host_port = rest[:path_start]
            path = rest[path_start:]

        port_start = host_port.rfind(":")
        if port_start >= 0:
            host = host_port[:port_start]
            port = int(host_port[port_start + 1 :])
        else:
            host = host_port

        return scheme, host, port, path

    def _decode_chunked(self, data):
        out = bytearray()
        offset = 0

        while True:
            line_end = data.find(b"\r\n", offset)
            if line_end < 0:
                break

            line = data[offset:line_end]
            semicolon = line.find(b";")
            if semicolon >= 0:
                line = line[:semicolon]

            size = int(line, 16)
            offset = line_end + 2

            if size == 0:
                break

            out.extend(data[offset : offset + size])
            offset += size + 2

        return bytes(out)

    def post_json(self, url, headers, body):
        scheme, host, port, path = self._parse_url(url)
        addr_info = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)[0]
        sock = socket.socket(addr_info[0], addr_info[1], addr_info[2])

        try:
            sock.connect(addr_info[-1])

            if scheme == "https":
                try:
                    import ssl as tls
                except ImportError:
                    try:
                        import tls
                    except ImportError:
                        raise RuntimeError("HTTPS needs TLS; use an HTTP proxy endpoint")

                context = tls.SSLContext(tls.PROTOCOL_TLS_CLIENT)
                if not self.tls_verify:
                    context.verify_mode = tls.CERT_NONE
                sock = context.wrap_socket(sock, server_hostname=host)

            body_bytes = body.encode()
            request = (
                "POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: esp-vision\r\n"
                "Connection: close\r\n"
                "Content-Length: %d\r\n"
            ) % (path, host, len(body_bytes))

            for name, value in headers.items():
                request += "%s: %s\r\n" % (name, value)

            request += "\r\n"

            sock.write(request.encode())
            sock.write(body_bytes)

            status_line = sock.readline().decode().strip()
            status_code = int(status_line.split(" ")[1])
            response_headers = {}

            while True:
                line = sock.readline()
                if line in (b"\r\n", b"\n", b""):
                    break

                name, value = line.decode().split(":", 1)
                response_headers[name.lower()] = value.strip()

            response_body = sock.read()
            if response_headers.get("transfer-encoding", "").lower() == "chunked":
                response_body = self._decode_chunked(response_body)

            return status_code, response_body.decode()
        finally:
            sock.close()


class OpenAI:
    def __init__(self, api_key, url, model, system_prompt, max_tokens, tls_verify=False):
        api_key = api_key.strip()
        if api_key == "your-api-key":
            api_key = ""

        if not api_key and url.find("api.openai.com") >= 0:
            raise RuntimeError("set API_KEY before using api.openai.com")

        self.url = url
        self.model = model
        self.system_prompt = system_prompt
        self.max_tokens = max_tokens
        self.http = _HttpClient(tls_verify=tls_verify)
        self.headers = {"Content-Type": "application/json"}

        if api_key:
            self.headers["Authorization"] = "Bearer " + api_key

    def describe_image(self, image_base64, prompt):
        body = self._build_payload(image_base64, prompt)
        print("request:", self.url)
        status, response = self.http.post_json(self.url, self.headers, body)
        print("status:", status)

        if status != 200:
            return response

        return self._extract_answer(response)

    def _build_payload(self, image_base64, prompt):
        return json.dumps(
            {
                "model": self.model,
                "messages": [
                    {
                        "role": "system",
                        "content": self.system_prompt,
                    },
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "image_url",
                                "image_url": {
                                    "url": "data:image/jpeg;base64," + image_base64,
                                },
                            },
                            {"type": "text", "text": prompt},
                        ],
                    },
                ],
                "max_tokens": self.max_tokens,
            }
        )

    def _extract_answer(self, text):
        data = json.loads(text)
        choices = data.get("choices", [])

        if not choices:
            return text

        first = choices[0]
        if "message" in first:
            return first["message"].get("content", "")
        if "text" in first:
            return first["text"]
        return text


def init_camera():
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QQVGA)
    sensor.skip_frames(time=1000)


def encode_jpeg_base64(img):
    jpg = img.to_jpeg(quality=JPEG_QUALITY)
    data = jpg.bytearray()
    print("jpeg bytes:", len(data))

    return binascii.b2a_base64(data).decode().strip()


def main():
    connect_wifi()
    button = Pin(BUTTON_PIN, Pin.IN, Pin.PULL_UP)
    init_camera()
    button_was_pressed = button.value() == BUTTON_PRESSED_LEVEL
    openai = OpenAI(API_KEY, API_URL, MODEL, SYSTEM_PROMPT, MAX_TOKENS, tls_verify=TLS_VERIFY)

    print("ready: press button on GPIO%d" % BUTTON_PIN)

    while True:
        img = sensor.snapshot()
        img.flush()

        button_is_pressed = button.value() == BUTTON_PRESSED_LEVEL
        should_upload = button_is_pressed and not button_was_pressed
        button_was_pressed = button_is_pressed

        if not should_upload:
            time.sleep_ms(FRAME_DELAY_MS)
            continue

        try:
            time.sleep_ms(20)
            if button.value() != BUTTON_PRESSED_LEVEL:
                continue

            image_base64 = encode_jpeg_base64(img)
            print(openai.describe_image(image_base64, PROMPT))
        except Exception as exc:
            print("cloud ai failed:", exc)
        finally:
            gc.collect()
            print("ready: press button on GPIO%d" % BUTTON_PIN)
            time.sleep_ms(FRAME_DELAY_MS)


main()
