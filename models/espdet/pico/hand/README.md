# ESPDet Pico Hand

## Overview

This 224x224 ESPDet Pico model detects human hands in RGB565 camera images with `espdl.ESPDet`.

## Usage

Use RGB565 images from `sensor.snapshot()`.

```python
import espdl

det = espdl.ESPDet("/flash/models/espdet/pico/hand/espdet_pico_224_224_hand.espdl", score=0.5, nms=0.7)
```
