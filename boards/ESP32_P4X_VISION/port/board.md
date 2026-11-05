The following firmware is applicable to the ESP32-P4X-VISION development board
based on ESP32-P4.

The current standard prebuilt firmware supports ESP32-P4 silicon revisions v3.1–v3.99. ESP32-P4 v1.3 requires a separately compiled and validated pre-v3 bootloader and application; the v3.x image must not be forced onto it.

This board must be equipped with at least 16 MiB external SPI Flash and PSRAM.
The default configuration enables the ESP32-P4 target, the ESP-VISION 16 MiB
flash layout, flash MSC, SDMMC slot 0, and SC101IOT DVP camera support. Display
support is left as a board-level placeholder until the LCD hardware is enabled.
