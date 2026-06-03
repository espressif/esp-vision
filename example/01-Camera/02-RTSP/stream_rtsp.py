# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Stream H.264 over RTSP using wired Ethernet (ESP32-P4-Function-EV-Board,
# IP101 PHY over RMII).
# Pull the stream on a host with:
#   ffplay rtsp://<board-ip>:8554/   (or open that URL in VLC / PotPlayer)

import network
import time
from machine import Pin

import sensor
import h264
import rtsp

WIDTH, HEIGHT, FPS = 320, 240, 30

# 1. Bring up Ethernet (IP101 over RMII; pins per the P4-Function-EV-Board).
lan = network.LAN(
    mdc=Pin(31),
    mdio=Pin(52),
    reset=Pin(51),
    phy_addr=1,
    phy_type=network.PHY_IP101,
)
lan.active(True)

print("waiting for ethernet link...")
for _ in range(100):  # up to ~10 s
    if lan.isconnected():
        break
    time.sleep_ms(100)

if not lan.isconnected():
    raise OSError("ethernet not connected - check cable / DHCP / RMII clock")

print("RTSP at rtsp://%s:8554/" % lan.ifconfig()[0])

# 2. Camera + encoder + RTSP server.
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

# Wired link has plenty of bandwidth, so use a high bitrate for sharp motion.
enc = h264.H264Encoder(WIDTH, HEIGHT, fps=FPS, bitrate=3_000_000, qp_max=32)
server = rtsp.RTSPServer(WIDTH, HEIGHT, fps=FPS, listen_port=8554)

# 3. Capture -> (optionally draw) -> encode -> push.
try:
    while True:
        img = sensor.snapshot()
        server.send(enc.encode(img))
finally:
    server.stop()
    enc.close()
