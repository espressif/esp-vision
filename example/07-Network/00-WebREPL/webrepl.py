# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# WebREPL over WiFi (STA mode) for ESP32_S3_EYE.
#
# Edit the constants below and run this script from the USB REPL. The USB REPL
# keeps working the whole time; WebREPL is an extra terminal mirrored onto
# ws://<ip>:8266, so after this runs you can use either one (or both).

import network
import time

SSID = "your-ssid"
PASSWORD = "your-wifi-password"
WEBREPL_PASSWORD = "min-4-chars"
TIMEOUT_S = 15

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.connect(SSID, PASSWORD)

deadline = time.ticks_add(time.ticks_ms(), TIMEOUT_S * 1000)
while not wlan.isconnected():
    if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
        raise RuntimeError("WiFi connect timed out")
    time.sleep_ms(200)

print("WiFi connected:", wlan.ifconfig()[0])

import webrepl

webrepl.start(password=WEBREPL_PASSWORD)
print("Connect a WebREPL client to ws://%s:8266" % wlan.ifconfig()[0])
