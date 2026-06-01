# ESPDet Models

## Overview

This directory stores ESP-DL object detection models.

Models are grouped by runtime API because different families use different postprocessors.

## Families

`pico/<model>/` uses `espdl.ESPDet`.

`yolo11n/` uses `espdl.YOLO11`.

`yolo11n-pose/` uses `espdl.YOLO11nPose`.

## Usage

Copy a model directory to `/sdcard/models/espdet/<family>/` or `/flash/models/espdet/<family>/`.

Use the matching Python API for that family.
