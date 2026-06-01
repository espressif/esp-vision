# ESPDet Pico Face

## Overview

This 224x224 ESPDet Pico model detects human faces in RGB565 camera images with `espdl.ESPDet`.

## Usage

Use RGB565 images from `sensor.snapshot()`.

```python
import espdl

det = espdl.ESPDet("/flash/models/espdet/pico/face/espdet_pico_224_224_face.espdl", score=0.5, nms=0.7)
```
