# NanoTrack V2 INT8

Three full-INT8 ESP-DL models for `espdl.NanoTrack`. The exports target ESP32-P4 / PIE V2 and are intended for P4/S31; S3 requires target-specific conversion and validation. They use the original all-INT8 experiment's weights and quantization, without embedded test inputs or outputs. The head's dynamic MatMul operands are packed for ESP-DL 3.3.6 by `build/experiments/nanotrack-v2/compat_espdl.py`. They are not the mixed INT16 accuracy experiment.

| File | Role | Size |
| --- | --- | ---: |
| `template_deploy.espdl` | 127x127 template backbone, used during selection | 328096 bytes |
| `search_deploy.espdl` | 255x255 search backbone, used every frame | 328096 bytes |
| `head_deploy.espdl` | Template/search feature matching and box prediction | 241728 bytes |
| **Total** | All three files required | **897920 bytes (about 877 KiB)** |

## Usage

Copy this folder to `/sdcard/nanotrack_v2/`, then run [`nanotrack.py`](../../example/03-Machine-Learning/00-ESP-DL/nanotrack.py) on ESP32-P4X-EYE or P4 Function EV Board. Place the target inside the center box and press BOOT to start tracking. The example's default paths already match this layout. On a board using `/flash`, change `MODEL_DIR` in the example.

```python
import espdl

tracker = espdl.NanoTrack(
    "/sdcard/nanotrack_v2/template_deploy.espdl",
    "/sdcard/nanotrack_v2/search_deploy.espdl",
    "/sdcard/nanotrack_v2/head_deploy.espdl",
)
```

The template model loads during `init()` and is freed after caching its features. Search and head models stay loaded during tracking. The files are not compiled into firmware, and their sizes do not represent peak runtime RAM usage. Device tracking accuracy and frame rate still require hardware validation.

Replace older copies of `head_deploy.espdl` on the board: the corrected SHA-256 is `3adc3c740ca191520f632d15e48574f8523987bb6f4775c4e7c5c2c495717ce9`. ESP-PPQ 1.3.11 emits `input1_layout=native_kn`, which ESP-DL 3.3.6 ignores and misreads as packed weights. This caused same-frame matching scores near 0.01–0.03; the compatible head scored 0.955–0.9999 on the same three captured-image ROIs on a P4 Function EV Board. This is a regression check, not a general tracking accuracy benchmark.

## Provenance and reproduction

Source: [HonglinChu/SiamTrackers, commit 248663f](https://github.com/HonglinChu/SiamTrackers/tree/248663fde6bf7c40190cf10ee396d5662919ecd3/NanoTrack/models/nanotrackv2). Conversion uses ESP-PPQ 1.3.11 with ESP32-P4 W8A8 quantization and KL calibration on 32 OTB David template/search pairs. The local experiment workspace, including the uv environment and PC accuracy results, lives under ignored `build/experiments/nanotrack-v2/` and is not shipped in a clean checkout. The local model-packing utility is `build/experiments/nanotrack-v2/compat_espdl.py`; ESP-DL 3.3.13 supports the deployed packed layout without changing these assets.

[`provenance.json`](provenance.json) records the original ONNX URLs and hashes, conversion settings and hashes of these exact exports. Each model also has a metadata sidecar with its byte count and SHA-256.

The upstream NanoTrack directory provides the Apache-2.0 text copied verbatim in [`UPSTREAM_LICENSE`](UPSTREAM_LICENSE). A separate declaration for these pretrained weight files has not been verified; weight and training-data redistribution terms must not be inferred from the code license. The repository's Apache-2.0 license does not override upstream asset terms.
