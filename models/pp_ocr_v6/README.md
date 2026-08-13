# PP-OCRv6 S8 Models

This directory contains the smallest and fastest PP-OCRv6 combination currently provided by ESP-DL:

- `pp_ocr_v6_det_s8.espdl`: 736 × 736 INT8 text detector.
- `pp_ocr_v6_rec_s8.espdl`: 48 × 320 INT8 short-text recognizer.
- `pp_ocr_v6_charset.dat`: 6906 UTF-8 dictionary entries stored as fixed four-byte, zero-padded records for low-overhead CTC decoding in MicroPython.

Both model files and the charset file are required by [`pp_ocr_v6.py`](../../example/03-Machine-Learning/00-ESP-DL/pp_ocr_v6.py). Copy these three files to the SD card root or adjust the three paths at the top of the example. The example starts in live-preview mode: press the configured active-low button once to recognize and hold the current frame, then press it again to resume live preview.

The Python implementation uses an axis-aligned crop for each detected text quadrilateral so resizing remains in the native `image` implementation. This keeps the example small and avoids slow per-pixel perspective warping in MicroPython, at the cost of lower recognition accuracy for strongly rotated or perspective-distorted text.

The assets were imported from `espressif/esp-dl` commit `cb769957aaff98f4207144c9cc6bf8078f56e64b`.
