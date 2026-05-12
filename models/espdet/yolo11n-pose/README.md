# YOLO11n Pose Models

## Overview

YOLO11n pose models are COCO human pose models for ESP-DL.

Use these models with `espdl.YOLO11nPose`.

Do not load YOLO11n pose models with `espdl.YOLO11` or `espdl.ESPDet`.

## Usage

The input image should be RGB565.

Copy the model to `/sdcard/models/espdet/yolo11n-pose/`, `/flash/models/espdet/yolo11n-pose/`, or another board storage path.

```python
import espdl

pose = espdl.YOLO11nPose("/flash/models/espdet/yolo11n-pose/espdet_yolo11n_pose_160_160_coco.espdl", score=0.35, nms=0.7, topk=5)
```
