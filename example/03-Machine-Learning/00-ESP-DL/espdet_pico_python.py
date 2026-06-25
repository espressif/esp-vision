# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import math
import sensor
import time


MODEL = "/sdcard/espdet_pico_224_224_face.espdl"

SCORE_THRESHOLD = 0.5
NMS_THRESHOLD = 0.7
TOPK = 10

DATA_TYPE_INT8 = 3
DATA_TYPE_INT16 = 5

# ESPDet Pico exports three feature map stages. Each stage uses an anchor point
# at x * stride + offset, y * stride + offset to decode box distances.
STAGES = (
    (0, 8, 4),
    (1, 16, 8),
    (2, 32, 16),
)


def dl_scale(exponent):
    # ESP-DL stores quantized tensor scale as a power-of-two exponent.
    if exponent > 0:
        return 1 << exponent
    return 1.0 / (1 << (-exponent))


def tensor_value(data, dtype, index):
    # Raw tensor bytes are returned from espdl.Model.predict(). Convert each
    # element back to a signed integer before applying the tensor scale.
    if dtype == DATA_TYPE_INT8:
        value = data[index]
        return value - 256 if value > 127 else value
    if dtype == DATA_TYPE_INT16:
        offset = index * 2
        value = data[offset] | (data[offset + 1] << 8)
        return value - 65536 if value > 32767 else value
    raise ValueError("unsupported tensor dtype")


def dequant(data, dtype, index, scale):
    return tensor_value(data, dtype, index) * scale


def sigmoid(value):
    if value >= 0:
        z = math.exp(-value)
        return 1.0 / (1.0 + z)
    z = math.exp(value)
    return z / (1.0 + z)


def clamp(value, low, high):
    return max(low, min(high, value))


def first_dict_value(items):
    for _, value in items.items():
        return value
    raise ValueError("empty tensor dictionary")


def letterbox_info(src_w, src_h, dst_w, dst_h):
    # The C preprocessor keeps aspect ratio by letterboxing. Python postprocess
    # must remove the same padding when mapping model coordinates to the image.
    scale = min(dst_w / src_w, dst_h / src_h)
    pad_x = (dst_w - int(src_w * scale)) // 2
    pad_y = (dst_h - int(src_h * scale)) // 2
    return scale, pad_x, pad_y


def image_x(value, scale, pad_x, width):
    return clamp(int((value - pad_x) / scale), 0, width - 1)


def image_y(value, scale, pad_y, height):
    return clamp(int((value - pad_y) / scale), 0, height - 1)


def tensor_data(outputs, name):
    # RawTensor: (name, shape, dtype, exponent, raw bytes).
    item = outputs[name]
    return item[1], item[2], item[3], item[4]


def iou(a, b):
    x1 = max(a[0], b[0])
    y1 = max(a[1], b[1])
    x2 = min(a[2], b[2])
    y2 = min(a[3], b[3])
    w = max(0, x2 - x1 + 1)
    h = max(0, y2 - y1 + 1)
    inter = w * h
    if inter <= 0:
        return 0.0
    area_a = (a[2] - a[0] + 1) * (a[3] - a[1] + 1)
    area_b = (b[2] - b[0] + 1) * (b[3] - b[1] + 1)
    return inter / (area_a + area_b - inter)


def nms(detections):
    # Keep high-score boxes and suppress boxes that overlap too much.
    detections.sort(key=lambda det: det[4], reverse=True)
    keep = []
    for det in detections:
        suppressed = False
        for selected in keep:
            if iou(det, selected) > NMS_THRESHOLD:
                suppressed = True
                break
        if not suppressed:
            keep.append(det)
            if len(keep) >= TOPK:
                break
    return keep


def decode_stage(outputs, stage, img_w, img_h, scale, pad_x, pad_y):
    # Decode one output stage. The formulas mirror ESP-DL's ESPDetPostProcessor.
    index, stride, offset = stage
    box_shape, box_dtype, box_exp, box_data = tensor_data(outputs, "box%d" % index)
    score_shape, score_dtype, score_exp, score_data = tensor_data(outputs, "score%d" % index)

    height = score_shape[1]
    width = score_shape[2]
    classes = score_shape[3]
    box_scale = dl_scale(box_exp)
    score_scale = dl_scale(score_exp)
    detections = []

    for y in range(height):
        for x in range(width):
            cell = y * width + x
            for category in range(classes):
                score = sigmoid(dequant(score_data, score_dtype, cell * classes + category, score_scale))
                if score < SCORE_THRESHOLD:
                    continue

                box_base = cell * box_shape[3]
                center_x = x * stride + offset
                center_y = y * stride + offset
                x1 = image_x(center_x - dequant(box_data, box_dtype, box_base + 0, box_scale) * stride, scale, pad_x, img_w)
                y1 = image_y(center_y - dequant(box_data, box_dtype, box_base + 1, box_scale) * stride, scale, pad_y, img_h)
                x2 = image_x(center_x + dequant(box_data, box_dtype, box_base + 2, box_scale) * stride, scale, pad_x, img_w)
                y2 = image_y(center_y + dequant(box_data, box_dtype, box_base + 3, box_scale) * stride, scale, pad_y, img_h)
                if x2 <= x1 or y2 <= y1:
                    continue
                detections.append((x1, y1, x2, y2, score, category))

    return detections


def decode_detections(outputs, img_w, img_h, model_w, model_h):
    scale, pad_x, pad_y = letterbox_info(img_w, img_h, model_w, model_h)
    detections = []
    for stage in STAGES:
        detections.extend(decode_stage(outputs, stage, img_w, img_h, scale, pad_x, pad_y))
    return nms(detections)


def draw_detections(img, detections):
    for x1, y1, x2, y2, score, category in detections:
        img.draw_rectangle(x1, y1, x2 - x1 + 1, y2 - y1 + 1, color=(255, 0, 0), thickness=2)
        img.draw_string(x1, max(0, y1 - 12), "%.2f:%d" % (score, category), color=(255, 0, 0))


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

print("model:", MODEL)
# espdl.Model exposes raw inputs/outputs so model-specific postprocess can stay
# in Python while ESP-DL still handles image preprocessing and inference.
model = espdl.Model(MODEL, mean=(0, 0, 0), std=(255, 255, 255), letterbox=True)
input_shape = first_dict_value(model.inputs())[1]
model_h = input_shape[1]
model_w = input_shape[2]
print("input:", input_shape)
print("outputs:", model.outputs())

try:
    while True:
        img = sensor.snapshot()
        outputs = model.predict(img)
        draw_detections(img, decode_detections(outputs, img.width(), img.height(), model_w, model_h))
        img.flush()
        time.sleep_ms(20)
finally:
    model.deinit()
