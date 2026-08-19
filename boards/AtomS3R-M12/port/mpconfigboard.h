#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "AtomS3R-M12"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME "ESP32-S3-PICO-1-N8R8"
#endif

#define MICROPY_HW_USB_MANUFACTURER_STRING "ESP-VISION"
#define MICROPY_HW_USB_PRODUCT_FS_STRING "ESP-VISION " MICROPY_HW_BOARD_NAME
#define MICROPY_HW_ENABLE_USBDEV (1)
#define MICROPY_HW_USB_CDC (1)
#define MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER (1)
// Present the software reset protocol as USB-Serial/JTAG so esptool selects
// its USBJTAGSerialReset sequence. This OTG device does not expose JTAG.
#define MICROPY_HW_USB_PID (0x1001)
#define MICROPY_HW_ESP_USB_SERIAL_JTAG (0)
#define MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE (0)
#define MICROPY_HW_ENABLE_UART_REPL (1)

#define MICROPY_HW_USB_MSC (1)
#define MICROPY_HW_USB_MSC_INTERFACE_STRING "AtomS3R-M12 Flash"
#define MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING "ESPVIS"
#define MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING "AtomS3R Flash"
#define MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING "1.00"

#define MICROPY_PY_ESPNOW (0)
#define MICROPY_HW_ENABLE_SDCARD (0)
#define MICROPY_PY_BLUETOOTH (0)

#ifndef MICROPY_PY_NETWORK_WLAN
#define MICROPY_PY_NETWORK_WLAN (1)
#endif

#define MICROPY_HW_I2C0_SCL (9)
#define MICROPY_HW_I2C0_SDA (12)

// TinyUSB's FreeRTOS OSAL can call portYIELD_FROM_ISR() while FreeRTOS.h is
// still including the Xtensa port headers. Define this trace hook before
// portmacro.h expands it; FreeRTOS.h normally provides the same no-op later.
#ifndef traceISR_EXIT_TO_SCHEDULER
#define traceISR_EXIT_TO_SCHEDULER()
#endif
