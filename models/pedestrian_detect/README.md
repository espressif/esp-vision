# Pedestrian Detection

## Overview

This directory contains ESP-DL's 224x224 INT8 PicoDet pedestrian detection model. ESP-VISION publishes the ESP32-P4 export as the shared model for ESP32-P4, ESP32-S31, and ESP32-S3 boards.

The matching runtime API is `espdl.PedestrianDetect`. Its defaults reproduce the upstream wrapper: score threshold `0.7`, NMS threshold `0.5`, top-k `10`, RGB mean `(0, 0, 0)`, RGB standard deviation `(1, 1, 1)`, and direct resize without letterboxing.

## Usage

Copy this directory to `/sdcard/pedestrian_detect/`, then load the model from that location:

```python
import espdl

det = espdl.PedestrianDetect(
    "/sdcard/pedestrian_detect/pedestrian_detect_pico.espdl"
)
```

`detect(image)` accepts RGB565 or grayscale images and returns `(x, y, width, height, score, category)` tuples. The model has one category, `person`, at index `0`.
