# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Stream camera frames as HTTP MJPEG over WiFi.
#
# Edit SSID/PASSWORD, run this script, then open:
#   http://<board-ip>/
#
# This is a browser-friendly JPEG stream, not RTSP. It is suitable for boards
# without H.264 support.

import errno
import network
import socket
import time

import sensor

SSID = "your-ssid"
PASSWORD = "your-wifi-password"

PORT = 80
JPEG_QUALITY = 35
FRAME_DELAY_MS = 50
CONNECT_TIMEOUT_S = 15
CONNECT_RETRY = 3
BOUNDARY = b"espvision"


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


def send_all(sock, data):
    view = memoryview(data)
    while view:
        sent = sock.send(view)
        if sent == 0:
            raise OSError("socket closed")
        view = view[sent:]


def send_index(client, ip):
    html = (
        "<!doctype html>"
        "<html>"
        "<head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP-VISION MJPEG</title>"
        "<style>"
        "body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center;}"
        "img{max-width:100vw;max-height:92vh;image-rendering:auto;}"
        "h3{font-weight:500;}"
        "</style>"
        "</head>"
        "<body>"
        "<h3>ESP-VISION MJPEG</h3>"
        "<img src='/stream'>"
        "<p>Stream: http://%s:%d/stream</p>"
        "</body>"
        "</html>"
    ) % (ip, PORT)

    body = html.encode()
    send_all(client, b"HTTP/1.0 200 OK\r\n")
    send_all(client, b"Content-Type: text/html\r\n")
    send_all(client, b"Cache-Control: no-cache\r\n")
    send_all(client, b"Connection: close\r\n")
    send_all(client, b"Content-Length: %d\r\n\r\n" % len(body))
    send_all(client, body)


def send_stream(client):
    send_all(client, b"HTTP/1.0 200 OK\r\n")
    send_all(client, b"Content-Type: multipart/x-mixed-replace; boundary=%s\r\n" % BOUNDARY)
    send_all(client, b"Cache-Control: no-cache\r\n")
    send_all(client, b"Pragma: no-cache\r\n")
    send_all(client, b"Connection: close\r\n\r\n")

    while True:
        img = sensor.snapshot()
        jpg = img.to_jpeg(quality=JPEG_QUALITY)
        data = jpg.bytearray()

        send_all(client, b"--%s\r\n" % BOUNDARY)
        send_all(client, b"Content-Type: image/jpeg\r\n")
        send_all(client, b"Content-Length: %d\r\n\r\n" % len(data))
        send_all(client, data)
        send_all(client, b"\r\n")

        time.sleep_ms(FRAME_DELAY_MS)


def main():
    wlan = connect_wifi()
    ip = wlan.ifconfig()[0]

    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=1000)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass

    server.bind(("0.0.0.0", PORT))
    server.listen(1)

    print("index: http://%s:%d/" % (ip, PORT))
    print("stream: http://%s:%d/stream" % (ip, PORT))

    while True:
        client, addr = server.accept()
        print("client:", addr)

        try:
            request = client.recv(512)
            if b"GET /stream" in request:
                send_stream(client)
            else:
                send_index(client, ip)
        except OSError as exc:
            print("client closed:", exc)
        finally:
            client.close()


main()
