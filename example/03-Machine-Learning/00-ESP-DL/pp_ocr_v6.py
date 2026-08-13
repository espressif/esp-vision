# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import espdl
import gc
import image
import math
import sensor
import time
from machine import Pin


DET_MODEL = "/sdcard/pp_ocr_v6_det_s8.espdl"
REC_MODEL = "/sdcard/pp_ocr_v6_rec_s8.espdl"
CHARSET = "/sdcard/pp_ocr_v6_charset.dat"

BUTTON_PIN = 35
BUTTON_PRESSED_LEVEL = 0

DET_THRESHOLD = 0.2
DET_BOX_THRESHOLD = 0.4
DET_UNCLIP_RATIO = 1.4
DET_MIN_SIZE = 3
REC_SCORE_THRESHOLD = 0.5
MAX_RESULTS = 16

DATA_TYPE_INT8 = 3
CHARSET_RECORD_SIZE = 4


# Convert an ESP-DL tensor exponent to its dequantization scale: real = quantized * scale.
def dl_scale(exponent):
    if exponent > 0:
        return 1 << exponent
    return 1.0 / (1 << (-exponent))


# Convert the unsigned 0..255 values exposed by bytes back to signed INT8.
def signed_int8(value):
    return value - 256 if value > 127 else value


# DET and REC each have one output, so return the first tensor in the dictionary.
def first_dict_value(items):
    for value in items.values():
        return value
    raise ValueError("empty tensor dictionary")


# Calculate the Euclidean distance used to measure a text box edge.
def distance(a, b):
    dx = a[0] - b[0]
    dy = a[1] - b[1]
    return math.sqrt(dx * dx + dy * dy)


# Order quadrilateral points as top-left, top-right, bottom-right, bottom-left.
def order_quad(points):
    top_left = min(points, key=lambda p: p[0] + p[1])
    bottom_right = max(points, key=lambda p: p[0] + p[1])
    remaining = [p for p in points if p is not top_left and p is not bottom_right]
    if len(remaining) != 2:
        return points
    if remaining[0][1] - remaining[0][0] <= remaining[1][1] - remaining[1][0]:
        top_right, bottom_left = remaining
    else:
        bottom_left, top_right = remaining
    return (top_left, top_right, bottom_right, bottom_left)


# Test whether a pixel center lies inside a rotated text box using ray casting.
def point_in_quad(x, y, box):
    inside = False
    previous = box[3]
    for current in box:
        if ((current[1] > y) != (previous[1] > y)) and (
            x < (previous[0] - current[0]) * (y - current[1]) / (previous[1] - current[1] + 1e-6) + current[0]
        ):
            inside = not inside
        previous = current
    return inside


# Average the text probability inside a box to reject weak DET candidates.
def box_score(data, scale, height, width, box):
    min_x = max(0, int(min(point[0] for point in box)))
    max_x = min(width - 1, int(max(point[0] for point in box) + 1))
    min_y = max(0, int(min(point[1] for point in box)))
    max_y = min(height - 1, int(max(point[1] for point in box) + 1))
    total = 0.0
    count = 0
    for y in range(min_y, max_y + 1):
        row = y * width
        for x in range(min_x, max_x + 1):
            if point_in_quad(x + 0.5, y + 0.5, box):
                total += signed_int8(data[row + x]) * scale
                count += 1
    return total / count if count else 0.0


# Expand a rotated box along its width and height so edge strokes are not cropped.
def expand_box(box, ratio):
    width = distance(box[0], box[1])
    height = distance(box[0], box[3])
    perimeter = 2.0 * (width + height)
    if width <= 1e-6 or height <= 1e-6 or perimeter <= 1e-6:
        return box

    offset = width * height * ratio / perimeter
    ux = (box[1][0] - box[0][0]) / width
    uy = (box[1][1] - box[0][1]) / width
    vx = (box[3][0] - box[0][0]) / height
    vy = (box[3][1] - box[0][1]) / height
    center_x = sum(point[0] for point in box) * 0.25
    center_y = sum(point[1] for point in box) * 0.25
    half_width = width * 0.5 + offset
    half_height = height * 0.5 + offset
    return (
        (center_x - ux * half_width - vx * half_height, center_y - uy * half_width - vy * half_height),
        (center_x + ux * half_width - vx * half_height, center_y + uy * half_width - vy * half_height),
        (center_x + ux * half_width + vx * half_height, center_y + uy * half_width + vy * half_height),
        (center_x - ux * half_width + vx * half_height, center_y - uy * half_width + vy * half_height),
    )


# Reproduce the centered letterbox dimensions and padding used by ImagePreprocessor.
def letterbox_info(src_width, src_height, dst_width, dst_height):
    scale = min(dst_width / src_width, dst_height / src_height)
    resized_width = max(1, int(src_width * scale))
    resized_height = max(1, int(src_height * scale))
    return (
        resized_width / src_width,
        resized_height / src_height,
        (dst_width - resized_width) // 2,
        (dst_height - resized_height) // 2,
    )


# Remove DET letterbox padding and map model coordinates back to the camera image.
def map_box_to_image(box, pred_width, pred_height, model_width, model_height, img_width, img_height):
    scale_x, scale_y, pad_x, pad_y = letterbox_info(img_width, img_height, model_width, model_height)
    mapped = []
    for point in box:
        x = int(round((point[0] * model_width / pred_width - pad_x) / scale_x))
        y = int(round((point[1] * model_height / pred_height - pad_y) / scale_y))
        mapped.append((max(0, min(img_width - 1, x)), max(0, min(img_height - 1, y))))
    return order_quad(mapped)


# Sort text boxes in reading order: top-to-bottom, then left-to-right on each line.
def sort_text_boxes(boxes):
    boxes.sort(key=lambda item: (item[0][0][1], item[0][0][0]))
    for index in range(1, len(boxes)):
        current = index
        while current > 0:
            this_box = boxes[current][0]
            previous_box = boxes[current - 1][0]
            if abs(this_box[0][1] - previous_box[0][1]) < 10 and this_box[0][0] < previous_box[0][0]:
                boxes[current], boxes[current - 1] = boxes[current - 1], boxes[current]
                current -= 1
            else:
                break
    return boxes


# DET post-processing: convert the per-pixel text map into quadrilaterals for REC.
def decode_detection(outputs, img_width, img_height, model_width, model_height):
    tensor = first_dict_value(outputs)
    shape, dtype, exponent, data = tensor[1], tensor[2], tensor[3], tensor[4]
    if dtype != DATA_TYPE_INT8 or len(shape) != 4 or shape[0] != 1 or shape[3] != 1:
        raise ValueError("PP-OCRv6 detection output must be INT8 NHWC [1,H,W,1]")

    height = shape[1]
    width = shape[2]
    scale = dl_scale(exponent)
    threshold_q = max(-128, min(127, int(round(DET_THRESHOLD / scale))))
    boxes = []

    # The tensor is signed INT8. Positive values remain in byte range 0..127,
    # while negative values wrap to 128..255. Wrapping it as a grayscale image
    # lets imlib find positive-probability components without a Python pixel scan.
    pred_image = image.Image(width, height, image.GRAYSCALE, buffer=data)
    blobs = pred_image.find_blobs(
        [(min(127, threshold_q + 1), 127)],
        x_stride=1,
        y_stride=1,
        area_threshold=DET_MIN_SIZE * DET_MIN_SIZE,
        pixels_threshold=DET_MIN_SIZE,
    )
    for blob in blobs:
        box = order_quad(blob.min_corners())
        short_side = min(distance(box[0], box[1]), distance(box[1], box[2]))
        if short_side < DET_MIN_SIZE:
            continue
        score = box_score(data, scale, height, width, box)
        if score < DET_BOX_THRESHOLD:
            continue
        expanded = expand_box(box, DET_UNCLIP_RATIO)
        mapped = map_box_to_image(
            expanded,
            width,
            height,
            model_width,
            model_height,
            img_width,
            img_height,
        )
        if min(distance(mapped[0], mapped[1]), distance(mapped[0], mapped[3])) <= DET_MIN_SIZE:
            continue
        boxes.append((mapped, score))
        if len(boxes) >= MAX_RESULTS:
            break
    return sort_text_boxes(boxes)


# Look up a CTC class in fixed four-byte records; class 0 is blank.
def charset_char(charset, class_id):
    start = class_id * CHARSET_RECORD_SIZE
    if start < CHARSET_RECORD_SIZE or start + CHARSET_RECORD_SIZE > len(charset):
        return "?"
    end = start + CHARSET_RECORD_SIZE
    while end > start and charset[end - 1] == 0:
        end -= 1
    return charset[start:end].decode("utf-8")


# REC post-processing: greedily decode CTC output and calculate mean confidence.
def decode_recognition(outputs, charset):
    tensor = first_dict_value(outputs)
    shape, dtype, exponent, data = tensor[1], tensor[2], tensor[3], tensor[4]
    if dtype != DATA_TYPE_INT8 or not shape:
        raise ValueError("PP-OCRv6 S8 recognition output must be INT8")

    class_count = shape[-1]
    time_steps = len(data) // class_count
    scale = dl_scale(exponent)
    previous = 0
    emitted = 0
    score_total = 0.0
    text = []
    for step in range(time_steps):
        offset = step * class_count
        best_class = 0
        best_value = signed_int8(data[offset])
        for class_id in range(1, class_count):
            value = signed_int8(data[offset + class_id])
            if value > best_value:
                best_value = value
                best_class = class_id
        if best_class == 0 or best_class == previous:
            previous = best_class
            continue

        text.append(charset_char(charset, best_class))
        exp_sum = 0.0
        best_logit = best_value * scale
        for class_id in range(class_count):
            exp_sum += math.exp(signed_int8(data[offset + class_id]) * scale - best_logit)
        score_total += 1.0 / exp_sum if exp_sum else 0.0
        emitted += 1
        previous = best_class
    return "".join(text), score_total / emitted if emitted else 0.0


# DET stage: locate text regions in the full camera frame.
class PPOCRDetector:
    def __init__(self, model_path):
        self.model = espdl.Model(
            model_path,
            mean=(123.675, 116.28, 103.53),
            std=(58.395, 57.12, 57.375),
            rgb_swap=True,
            letterbox=True,
            pad=(127, 127, 127),
        )

        input_shape = first_dict_value(self.model.inputs())[1]
        self.input_height = input_shape[1]
        self.input_width = input_shape[2]

    # DET preprocessing is configured above and runs inside Model.predict().
    # DET post-processing converts the output probability map into text boxes.
    def detect(self, img):
        outputs = self.model.predict(img)
        try:
            return decode_detection(
                outputs,
                img.width(),
                img.height(),
                self.input_width,
                self.input_height,
            )
        finally:
            del outputs
            gc.collect()

    def deinit(self):
        self.model.deinit()


# REC stage: crop and recognize every text region returned by DET.
class PPOCRRecognizer:
    def __init__(self, model_path, charset_path):
        with open(charset_path, "rb") as charset_file:
            self.charset = charset_file.read()
        if len(self.charset) % CHARSET_RECORD_SIZE:
            raise ValueError("invalid PP-OCRv6 charset")

        self.model = espdl.Model(
            model_path,
            mean=(127.5, 127.5, 127.5),
            std=(127.5, 127.5, 127.5),
            rgb_swap=True,
            letterbox=False,
        )

        input_shape = first_dict_value(self.model.inputs())[1]
        self.input_height = input_shape[1]
        self.input_width = input_shape[2]
        self.input_image = image.Image(self.input_width, self.input_height, image.RGB565)

    # Crop the quadrilateral's bounding box, then rotate/resize it for the 320x48 REC input.
    def _prepare_input(self, img, box):
        min_x = max(0, min(point[0] for point in box))
        max_x = min(img.width() - 1, max(point[0] for point in box))
        min_y = max(0, min(point[1] for point in box))
        max_y = min(img.height() - 1, max(point[1] for point in box))
        width = max_x - min_x + 1
        height = max_y - min_y + 1

        self.input_image.draw_rectangle(
            0,
            0,
            self.input_width,
            self.input_height,
            color=(128, 128, 128),
            fill=True,
        )
        if height / width >= 1.5:
            scale = min(self.input_height / width, self.input_width / height)
            self.input_image.draw_image(
                img,
                0,
                0,
                roi=(min_x, min_y, width, height),
                x_scale=scale,
                y_scale=scale,
                hint=image.BILINEAR | image.ROTATE_90,
            )
        else:
            scale = min(self.input_height / height, self.input_width / width)
            self.input_image.draw_image(
                img,
                0,
                0,
                roi=(min_x, min_y, width, height),
                x_scale=scale,
                y_scale=scale,
                hint=image.BILINEAR,
            )

    # REC preprocessing crops/resizes one detected box, then Model.predict()
    # applies the configured normalization before inference. CTC decoding follows.
    def recognize_box(self, img, box):
        self._prepare_input(img, box)
        outputs = self.model.predict(self.input_image)
        try:
            return decode_recognition(outputs, self.charset)
        finally:
            del outputs

    # Run the complete REC stage independently for all DET boxes.
    def recognize(self, img, boxes):
        results = []
        for box, det_score in boxes:
            text, rec_score = self.recognize_box(img, box)
            if rec_score >= REC_SCORE_THRESHOLD:
                results.append((box, text, rec_score, det_score))
        gc.collect()
        return results

    def deinit(self):
        self.model.deinit()


# Draw red boxes and yellow ASCII labels; print the complete UTF-8 text to the terminal.
def draw_results(img, results):
    for box, text, rec_score, det_score in results:
        for index in range(4):
            start = box[index]
            end = box[(index + 1) % 4]
            img.draw_line(start[0], start[1], end[0], end[1], color=(255, 0, 0), thickness=2)

        preview_text = "".join(char if 32 <= ord(char) <= 126 else "?" for char in text)
        if preview_text:
            text_x = max(0, min(img.width() - 1, box[0][0]))
            text_y = max(0, box[0][1] - 10)
            max_chars = max(1, (img.width() - text_x) // 8)
            img.draw_string(text_x, text_y, preview_text[:max_chars], color=(255, 255, 0))
        print("text=%s rec=%.3f det=%.3f" % (text, rec_score, det_score))


# Debounce the active-low button and wait for release so one press triggers one transition.
def button_pressed(button):
    if button.value() != BUTTON_PRESSED_LEVEL:
        return False
    time.sleep_ms(20)
    if button.value() != BUTTON_PRESSED_LEVEL:
        return False
    while button.value() == BUTTON_PRESSED_LEVEL:
        time.sleep_ms(10)
    return True


sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

button = Pin(BUTTON_PIN, Pin.IN, Pin.PULL_UP)
detector = PPOCRDetector(DET_MODEL)
recognizer = PPOCRRecognizer(REC_MODEL, CHARSET)
showing_result = False
print("preview: press GPIO%d for OCR" % BUTTON_PIN)
try:
    while True:
        if showing_result:
            if button_pressed(button):
                showing_result = False
                print("preview resumed")
            else:
                time.sleep_ms(10)
            continue

        frame = sensor.snapshot()
        if button_pressed(button):
            print("OCR started")

            # Step 1: DET finds and orders all text regions in the frame.
            text_boxes = detector.detect(frame)

            # Step 2: REC crops and recognizes each detected text region.
            results = recognizer.recognize(frame, text_boxes)

            draw_results(frame, results)
            showing_result = True
            print("OCR done: press GPIO%d to resume preview" % BUTTON_PIN)
        frame.flush()
        time.sleep_ms(20)
finally:
    detector.deinit()
    recognizer.deinit()
