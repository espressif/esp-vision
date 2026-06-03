# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Record a short H.264 clip from the camera to the SD card (ESP32-P4).
# Play it back on a host with:
#   ffplay out.h264
#   ffmpeg -framerate 15 -i out.h264 -c copy out.mp4

import sensor
import h264

FRAME_COUNT = 300
OUT_PATH = "/sdcard/out.h264"

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

# Size the encoder from the first frame so it matches the camera output.
img = sensor.snapshot()
enc = h264.H264Encoder(img.width(), img.height(), fps=15)

with open(OUT_PATH, "wb") as f:
    f.write(enc.encode(img))
    for i in range(FRAME_COUNT - 1):
        f.write(enc.encode(sensor.snapshot()))

enc.close()
print("wrote", FRAME_COUNT, "frames to", OUT_PATH)
