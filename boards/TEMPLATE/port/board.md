# ESP-VISION TEMPLATE

This board package is a bring-up template. Copy the whole `boards/TEMPLATE` directory (including its `port/` subdirectory) to a new board name, then update the target, flash layout, USB strings, pins, and board-specific camera/display/SD card implementations.

The template intentionally uses direct board-specific implementations and does not enable ESP Board Manager. Boards are not required to use Board Manager. If a copied board is converted to the Board Manager model, remove its board-level `display.c`; the shared `platform/display.c` supplies the `esp_vision_board_display_*` hooks when `ESP_VISION_USE_BOARD_MANAGER` is enabled.
