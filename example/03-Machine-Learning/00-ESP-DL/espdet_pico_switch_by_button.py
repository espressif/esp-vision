# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Switch the recognition model with a button press.
# Each press unloads the current model and loads the next one in the list.
# Note: ESP-DL models use a lot of PSRAM, so on every switch the old model must
# be deinit()-ed to free memory before the new one is created.

import espdl
import sensor
import time
from machine import Pin


# User button on the ESP32-P4X-EYE board is wired to GPIO3 (active low: pressed reads 0).
BUTTON_PIN = 3
BUTTON_PRESSED_LEVEL = 0  # level read while the button is held down


# Only ESPDet (PicoDet) models are used here: they are small and fast.
# YOLO11 / pose models are intentionally left out because they run too slowly.
# Each entry: (name, model path). All share the same espdl.ESPDet runtime.
MODELS = (
    ("face", "/sdcard/espdet_pico_224_224_face.espdl"),
    ("hand", "/sdcard/espdet_pico_224_224_hand.espdl"),
    ("cat_dog", "/sdcard/espdet_pico_224_224_cat_dog.espdl"),
)

SCORE = 0.5
NMS = 0.7


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

button = Pin(BUTTON_PIN, Pin.IN, Pin.PULL_UP)

current_index = -1
detector = None


def load(index):
    """Unload the current model and load model number `index`."""
    global detector, current_index
    if detector is not None:
        detector.deinit()
        detector = None
    name, path = MODELS[index]
    print("loading:", name)
    detector = espdl.ESPDet(path, score=SCORE, nms=NMS)
    current_index = index


def button_pressed():
    """Debounced edge detection. Returns True once per physical press."""
    if button.value() != BUTTON_PRESSED_LEVEL:
        return False
    time.sleep_ms(20)  # debounce
    if button.value() != BUTTON_PRESSED_LEVEL:
        return False
    # Wait for release so a long hold does not retrigger.
    while button.value() == BUTTON_PRESSED_LEVEL:
        time.sleep_ms(10)
    return True


load(0)

try:
    while True:
        if button_pressed():
            load((current_index + 1) % len(MODELS))

        img = sensor.snapshot()
        for result in detector.detect(img):
            x, y, w, h, score, category = result[:6]
            img.draw_rectangle(x, y, w, h, color=(255, 0, 0), thickness=2)
            img.draw_string(x, max(0, y - 12), "%.2f:%d" % (score, category), color=(255, 0, 0))
        img.draw_string(2, 2, MODELS[current_index][0], color=(0, 255, 0))
        img.flush()
        time.sleep_ms(20)
finally:
    if detector is not None:
        detector.deinit()
