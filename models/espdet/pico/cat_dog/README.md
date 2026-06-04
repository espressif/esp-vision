# ESPDet Pico Cat Dog

## Overview

This 224x224 ESPDet Pico model detects cats and dogs in RGB565 camera images with `espdl.ESPDet`.

The training dataset is derived from the [Oxford-IIIT Pet dataset](https://universe.roboflow.com/machine-learning-2s6qy/oxford-iiit-pet-uvpd0) published on Roboflow Universe.

## Usage

Use RGB565 images from `sensor.snapshot()`.

```python
import espdl

det = espdl.ESPDet("/flash/models/espdet/pico/cat_dog/espdet_pico_224_224_cat_dog.espdl", score=0.5, nms=0.7)
```
