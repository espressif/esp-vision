# ESPDet Pico Cat

## Overview

This 224x224 ESPDet Pico model detects cats in RGB565 camera images with `espdl.ESPDet`.

## Usage

Use RGB565 images from `sensor.snapshot()`.

```python
import espdl

det = espdl.ESPDet("/flash/models/espdet/pico/cat/espdet_pico_224_224_cat.espdl", score=0.5, nms=0.7)
```
