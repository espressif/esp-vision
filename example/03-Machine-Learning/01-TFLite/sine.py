# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import math
import image
import os
import struct
import tflite
import time


MODEL = "/sdcard/sine.tflite"
WIDTH = 320
HEIGHT = 240
PLOT_X = 24
PLOT_Y = 36
PLOT_W = 272
PLOT_H = 156
STEPS = 64
X_RANGE = 2.0 * math.pi

DTYPE_FLOAT = ord("f")
DTYPE_INT8 = ord("b")
DTYPE_UINT8 = ord("B")


def find_model():
    try:
        os.stat(MODEL)
        return MODEL
    except OSError:
        raise OSError("copy sine.tflite to /sdcard/")


def clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


class ScalarPreprocessor:
    def __init__(self, input_scale, input_zero_point):
        self.scale = input_scale
        self.zero_point = input_zero_point

    def input_filler(self, value):
        def fill(buf, shape, dtype):
            if dtype == DTYPE_FLOAT:
                struct.pack_into("<f", buf, 0, value)
            elif dtype == DTYPE_INT8:
                q = int(value / self.scale + self.zero_point)
                buf[0] = clamp(q, -128, 127) & 0xFF
            elif dtype == DTYPE_UINT8:
                q = int(value / self.scale + self.zero_point)
                buf[0] = clamp(q, 0, 255)
            else:
                raise ValueError("sine example expects a scalar numeric input")

        return fill


class ScalarPostprocessor:
    def __init__(self, output_scale, output_zero_point):
        self.scale = output_scale
        self.zero_point = output_zero_point

    def decode(self, output):
        value = self._first(output)
        return (value - self.zero_point) * self.scale

    def _first(self, output):
        try:
            return output.reshape((1,))[0]
        except (AttributeError, ValueError):
            return output[0]


def plot_x(index):
    return PLOT_X + int(index * (PLOT_W - 1) / (STEPS - 1))


def plot_y(value):
    value = max(-1.2, min(1.2, value))
    return PLOT_Y + int((1.2 - value) * (PLOT_H - 1) / 2.4)


def draw_axes(img):
    img.clear()
    img.draw_rectangle(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, color=(80, 80, 80), thickness=1)
    img.draw_line(PLOT_X, plot_y(0.0), PLOT_X + PLOT_W - 1, plot_y(0.0), color=(60, 60, 60), thickness=1)
    img.draw_string(8, 6, "TFLite sine", color=(255, 255, 255))
    img.draw_string(PLOT_X, HEIGHT - 34, "gray: math.sin", color=(160, 160, 160))
    img.draw_string(PLOT_X + 128, HEIGHT - 34, "green: tflite", color=(0, 255, 0))


def draw_curve(img, values, color, thickness):
    for i in range(1, len(values)):
        img.draw_line(
            plot_x(i - 1),
            plot_y(values[i - 1]),
            plot_x(i),
            plot_y(values[i]),
            color=color,
            thickness=thickness,
        )


def draw_predicted_curve(img, values, valid_points, color, thickness):
    for i in range(1, len(values)):
        if valid_points[i - 1] and valid_points[i]:
            img.draw_line(
                plot_x(i - 1),
                plot_y(values[i - 1]),
                plot_x(i),
                plot_y(values[i]),
                color=color,
                thickness=thickness,
            )


def run_model(model, preprocess, postprocess, x):
    output = model.predict([preprocess.input_filler(x)])[0]
    return postprocess.decode(output)


model_path = find_model()
model = tflite.Model(model_path)
print("model:", model_path)
print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)

preprocess = ScalarPreprocessor(model.input_scale[0], model.input_zero_point[0])
postprocess = ScalarPostprocessor(model.output_scale[0], model.output_zero_point[0])

try:
    img = image.Image(WIDTH, HEIGHT, image.RGB565)
    expected_values = []
    predicted_values = [0.0] * STEPS
    valid_points = [False] * STEPS
    frame = 0

    for i in range(STEPS):
        x = X_RANGE * i / (STEPS - 1)
        expected_values.append(math.sin(x))

    while True:
        cursor = frame % STEPS
        if cursor == 0:
            valid_points = [False] * STEPS

        x = X_RANGE * cursor / (STEPS - 1)
        expected = expected_values[cursor]
        predicted = run_model(model, preprocess, postprocess, x)
        predicted_values[cursor] = predicted
        valid_points[cursor] = True

        draw_axes(img)
        draw_curve(img, expected_values, (120, 120, 120), 1)
        draw_predicted_curve(img, predicted_values, valid_points, (0, 255, 0), 2)
        img.draw_circle(plot_x(cursor), plot_y(predicted), 4, color=(255, 255, 0), thickness=1, fill=True)
        img.draw_string(8, 206, "x %.2f  pred %.3f  sin %.3f" % (x, predicted, expected), color=(255, 255, 255))
        img.flush()

        if frame % STEPS == 0:
            print("pred: %.3f sin: %.3f err: %.3f" % (predicted, expected, abs(predicted - expected)))

        frame = (frame + 1) % 1000000
        time.sleep_ms(80)
finally:
    model.deinit()
