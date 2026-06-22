# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import gc
import image
import os
import sensor
import tflite
import time


MODEL = "/sdcard/person_detect.tflite"
PERSON_THRESHOLD = 0.6
DEBUG_EVERY = 15

DTYPE_INT8 = ord("b")
DTYPE_UINT8 = ord("B")


def find_model():
    try:
        os.stat(MODEL)
        return MODEL
    except OSError:
        raise OSError("copy person_detect.tflite to /sdcard/")


class PersonPreprocessor:
    def __init__(self, input_shape):
        if len(input_shape) != 4 or input_shape[3] != 1:
            raise ValueError("person detection expects NHWC grayscale input")
        self.height = input_shape[1]
        self.width = input_shape[2]

    def prepare(self, img):
        crop_side = min(img.width(), img.height())
        crop_x = (img.width() - crop_side) // 2
        crop_y = (img.height() - crop_side) // 2
        resized = img.copy(
            roi=(crop_x, crop_y, crop_side, crop_side),
            x_scale=self.width / crop_side,
            y_scale=self.height / crop_side,
            hint=image.BILINEAR,
            copy=True,
        )
        return resized, (crop_x, crop_y, crop_side, crop_side)

    def input_filler(self, img):
        pixels = img.bytearray()

        def fill(buf, shape, dtype):
            total = shape[1] * shape[2] * shape[3]
            if dtype == DTYPE_INT8:
                for i in range(total):
                    buf[i] = pixels[i] ^ 0x80
            elif dtype == DTYPE_UINT8:
                for i in range(total):
                    buf[i] = pixels[i]
            else:
                raise ValueError("person detection expects an int8 or uint8 input tensor")

        return fill


class PersonPostprocessor:
    def __init__(self, output_scale, output_zero_point, threshold):
        self.scale = output_scale
        self.zero_point = output_zero_point
        self.threshold = threshold

    def decode(self, output):
        output = self._flat_output(output)
        raw_not_person = output[0]
        raw_person = output[1]
        not_person = self._score(raw_not_person)
        person = self._score(raw_person)
        is_person = person >= not_person
        threshold_ok = person >= self.threshold
        return raw_not_person, raw_person, not_person, person, is_person, threshold_ok

    def _flat_output(self, output):
        try:
            return output.reshape((2,))
        except (AttributeError, ValueError):
            return output

    def _score(self, value):
        return (value - self.zero_point) * self.scale


model_path = find_model()
model = tflite.Model(model_path)
print("model:", model_path)
print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)

preprocess = PersonPreprocessor(model.input_shape[0])
postprocess = PersonPostprocessor(
    model.output_scale[0],
    model.output_zero_point[0],
    PERSON_THRESHOLD,
)

sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

frame = 0

try:
    while True:
        frame += 1
        start = time.ticks_ms()
        img = sensor.snapshot()

        input_img, input_roi = preprocess.prepare(img)
        output = model.predict([preprocess.input_filler(input_img)])[0]
        raw_not_person, raw_person, not_person, person, is_person, threshold_ok = postprocess.decode(output)

        elapsed = time.ticks_diff(time.ticks_ms(), start)
        img.draw_rectangle(input_roi[0], input_roi[1], input_roi[2], input_roi[3], color=255)
        if is_person:
            label = "person %.2f" % person
            color = 255
        else:
            label = "no person %.2f" % not_person
            color = 180

        img.draw_string(2, 2, label, color=color)
        img.draw_string(2, 14, "%d ms" % elapsed, color=color)
        if threshold_ok:
            img.draw_string(2, 26, "threshold ok", color=255)
        img.flush()

        if frame % DEBUG_EVERY == 0:
            print(
                "raw:",
                raw_not_person,
                raw_person,
                "score:",
                "%.3f" % not_person,
                "%.3f" % person,
            )

        gc.collect()
finally:
    model.deinit()
