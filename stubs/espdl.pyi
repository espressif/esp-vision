# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Sequence

from image import Image

#: Three-channel value in RGB order.
Float3 = tuple[float, float, float] | Sequence[float]
#: Detection result tuple: (x, y, w, h, score, category).
Detection = tuple[int, int, int, int, float, int]
#: COCO pose keypoint tuple: (x, y). Missing keypoints are returned as (0, 0).
PoseKeypoint = tuple[int, int]
#: Pose result tuple: (x, y, w, h, score, category, keypoints).
PoseDetection = tuple[int, int, int, int, float, int, list[PoseKeypoint]]
#: Classification result tuple: (label, score).
Classification = tuple[str, float]


#: Preload an ESP-DL model file.
#: path: model path, for example "/sdcard/model.espdl" or "/flash/model.espdl".
#: profile: True enables ESP-DL profiling output when supported.
def load_model(path: str, *, profile: bool = False) -> bool: ...


#: ESP-DL object detection wrapper for ESPDet models.
class ESPDet:
    #: Create a detector from an .espdl model.
    #: path: model path.
    #: score: optional confidence threshold.
    #: nms: optional non-maximum suppression threshold.
    #: mean: optional RGB mean values for preprocessing.
    #: std: optional RGB standard deviation values for preprocessing.
    def __init__(
        self,
        path: str,
        *,
        score: float | None = None,
        nms: float | None = None,
        mean: Float3 | None = None,
        std: Float3 | None = None,
    ) -> None: ...
    def __del__(self) -> None: ...
    #: Release model resources.
    def deinit(self) -> None: ...
    #: Run object detection on an image.
    #: image: RGB565 or grayscale image.
    def detect(self, image: Image) -> list[Detection]: ...
    #: Update detector thresholds.
    #: score: new confidence threshold, or None to keep current value.
    #: nms: new NMS threshold, or None to keep current value.
    def set_thresholds(self, *, score: float | None = None, nms: float | None = None) -> ESPDet: ...


#: ESP-DL YOLO11 object detection wrapper.
class YOLO11:
    #: Create a YOLO11 detector from an .espdl model.
    #: path: model path.
    #: score: optional confidence threshold.
    #: nms: optional non-maximum suppression threshold.
    #: topk: maximum number of detections returned per frame.
    #: mean: optional RGB mean values for preprocessing.
    #: std: optional RGB standard deviation values for preprocessing.
    def __init__(
        self,
        path: str,
        *,
        score: float | None = None,
        nms: float | None = None,
        topk: int = 10,
        mean: Float3 | None = None,
        std: Float3 | None = None,
    ) -> None: ...
    def __del__(self) -> None: ...
    #: Release model resources.
    def deinit(self) -> None: ...
    #: Run object detection on an image.
    #: image: RGB565 or grayscale image.
    def detect(self, image: Image) -> list[Detection]: ...
    #: Update detector thresholds.
    #: score: new confidence threshold, or None to keep current value.
    #: nms: new NMS threshold, or None to keep current value.
    def set_thresholds(self, *, score: float | None = None, nms: float | None = None) -> YOLO11: ...


#: ESP-DL YOLO11n COCO pose wrapper.
class YOLO11nPose:
    #: Create a YOLO11n pose detector from an .espdl model.
    #: path: model path.
    #: score: optional confidence threshold.
    #: nms: optional non-maximum suppression threshold.
    #: topk: maximum number of pose results returned per frame.
    #: mean: optional RGB mean values for preprocessing.
    #: std: optional RGB standard deviation values for preprocessing.
    def __init__(
        self,
        path: str,
        *,
        score: float | None = None,
        nms: float | None = None,
        topk: int = 10,
        mean: Float3 | None = None,
        std: Float3 | None = None,
    ) -> None: ...
    def __del__(self) -> None: ...
    #: Release model resources.
    def deinit(self) -> None: ...
    #: Run COCO pose detection on an image.
    #: image: RGB565 or grayscale image.
    #: Returns 17 COCO keypoints for each person.
    def detect(self, image: Image) -> list[PoseDetection]: ...
    #: Update detector thresholds.
    #: score: new confidence threshold, or None to keep current value.
    #: nms: new NMS threshold, or None to keep current value.
    def set_thresholds(self, *, score: float | None = None, nms: float | None = None) -> YOLO11nPose: ...


#: ESP-DL ImageNet classification wrapper.
class ImageNetCls:
    #: Create a classifier from an .espdl model.
    #: path: model path.
    #: topk: maximum number of classes returned.
    #: score: optional minimum score threshold.
    #: mean: optional RGB mean values for preprocessing.
    #: std: optional RGB standard deviation values for preprocessing.
    #: softmax: True applies softmax to output scores.
    def __init__(
        self,
        path: str,
        *,
        topk: int = 5,
        score: float | None = None,
        mean: Float3 | None = None,
        std: Float3 | None = None,
        softmax: bool = True,
    ) -> None: ...
    def __del__(self) -> None: ...
    #: Release model resources.
    def deinit(self) -> None: ...
    #: Run image classification on an image.
    #: image: RGB565 or grayscale image.
    def classify(self, image: Image) -> list[Classification]: ...
    #: Update classifier thresholds.
    #: topk: new maximum result count, or None to keep current value.
    #: score: new minimum score, or None to keep current value.
    def set_thresholds(self, *, topk: int | None = None, score: float | None = None) -> ImageNetCls: ...
