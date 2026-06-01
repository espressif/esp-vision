# ESPDet Pico Dog

## Overview

This 224x224 ESPDet Pico model detects dogs in RGB565 camera images with `espdl.ESPDet`.

## Usage

Use RGB565 images from `sensor.snapshot()`.

```python
import espdl

det = espdl.ESPDet("/flash/models/espdet/pico/dog/espdet_pico_224_224_dog.espdl", score=0.5, nms=0.7)
```
