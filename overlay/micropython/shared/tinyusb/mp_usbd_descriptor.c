/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Damien P. George
 * Copyright (c) 2022 Blake W. Felt & Angus Gratton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/mpconfig.h"

#if MICROPY_HW_ENABLE_USBDEV

#include "tusb.h"
#include "mp_usbd.h"

#define USBD_CDC_CMD_MAX_SIZE (8)
#define USBD_CDC_IN_OUT_MAX_SIZE ((CFG_TUD_MAX_SPEED == OPT_MODE_HIGH_SPEED) ? 512 : 64)
#define USBD_MSC_IN_OUT_MAX_SIZE ((CFG_TUD_MAX_SPEED == OPT_MODE_HIGH_SPEED) ? 512 : 64)

const tusb_desc_device_t mp_usbd_builtin_desc_dev = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = MICROPY_HW_USB_VID,
    #ifdef USB_TUSB_PID
    .idProduct = MICROPY_HW_USB_PID | (CFG_TUD_VIDEO ? 0x20 : 0),
    #else
    .idProduct = MICROPY_HW_USB_PID,
    #endif
    .bcdDevice = CFG_TUD_VIDEO ? 0x0200 : 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t mp_usbd_builtin_desc_cfg[MP_USBD_BUILTIN_DESC_CFG_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_BUILTIN_MAX, USBD_STR_0, MP_USBD_BUILTIN_DESC_CFG_LEN,
        0, USBD_MAX_POWER_MA),

    #if CFG_TUD_CDC
    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),
    #endif
    #if CFG_TUD_MSC
    TUD_MSC_DESCRIPTOR(USBD_ITF_MSC, USBD_STR_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, USBD_MSC_IN_OUT_MAX_SIZE),
    #endif
    #if CFG_TUD_VIDEO
    TUD_VIDEO_DESC_IAD(USBD_ITF_UVC, 2, USBD_STR_UVC),
    TUD_VIDEO_DESC_STD_VC(USBD_ITF_UVC, 0, USBD_STR_UVC),
    TUD_VIDEO_DESC_CS_VC(0x0150, TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN,
        27000000, USBD_ITF_UVC + 1),
    TUD_VIDEO_DESC_CAMERA_TERM(1, 0, 0, 0, 0, 0, 0),
    TUD_VIDEO_DESC_OUTPUT_TERM(2, VIDEO_TT_STREAMING, 0, 1, 0),
    TUD_VIDEO_DESC_STD_VS(USBD_ITF_UVC + 1, 0, 0, USBD_STR_UVC),
    TUD_VIDEO_DESC_CS_VS_INPUT(1,
        TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN +
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN,
        USBD_UVC_EP_IN, 0, 2, 0, 0, 0, 0),
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(1, 1, 0, 1, 0, 0, 0, 0),
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(1, 0, 320, 240,
        8 * 1024 * 10, 48 * 1024 * 8 * 10, 48 * 1024,
        1000000, 1000000, 1000000, 1000000),
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709,
        VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M),
    TUD_VIDEO_DESC_STD_VS(USBD_ITF_UVC + 1, 1, 1, USBD_STR_UVC),
    TUD_VIDEO_DESC_EP_ISO(USBD_UVC_EP_IN, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, 1),
    #endif
};

// The builtin array uses the controller's maximum speed. Keep the full-speed
// variant separate so a high-speed controller can enumerate through an FS hub.
const uint8_t *mp_usbd_builtin_configuration(bool other_speed) {
    static uint8_t fs_cfg[MP_USBD_BUILTIN_DESC_CFG_LEN];
    static uint8_t other_cfg[MP_USBD_BUILTIN_DESC_CFG_LEN];
    bool high = tud_speed_get() == TUSB_SPEED_HIGH;
    if (other_speed) {
        high = !high;
    }
    if (high && !other_speed) {
        return mp_usbd_builtin_desc_cfg;
    }
    uint8_t *cfg = other_speed ? other_cfg : fs_cfg;
    memcpy(cfg, mp_usbd_builtin_desc_cfg, sizeof(fs_cfg));
    cfg[1] = other_speed ? TUSB_DESC_OTHER_SPEED_CONFIG : TUSB_DESC_CONFIGURATION;
    for (size_t pos = 0; pos < sizeof(fs_cfg); pos += cfg[pos]) {
        if (cfg[pos + 1] == TUSB_DESC_ENDPOINT && (cfg[pos + 3] & 3) == TUSB_XFER_BULK) {
            cfg[pos + 4] = high ? 0 : 64;
            cfg[pos + 5] = high ? 2 : 0;
        }
    }
    return cfg;
}

#if TUD_OPT_HIGH_SPEED
const uint8_t *tud_descriptor_device_qualifier_cb(void) {
    static const tusb_desc_device_qualifier_t qualifier = {
        .bLength = sizeof(tusb_desc_device_qualifier_t),
        .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
        .bcdUSB = 0x0200,
        .bDeviceClass = TUSB_CLASS_MISC,
        .bDeviceSubClass = MISC_SUBCLASS_COMMON,
        .bDeviceProtocol = MISC_PROTOCOL_IAD,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .bNumConfigurations = 1,
    };
    if (tud_descriptor_device_cb() != (const uint8_t *)&mp_usbd_builtin_desc_dev) {
        return NULL; // Runtime descriptors require their own speed contract.
    }
    return (const uint8_t *)&qualifier;
}

const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
    if (index != 0 || tud_descriptor_device_cb() != (const uint8_t *)&mp_usbd_builtin_desc_dev) {
        return NULL;
    }
    return mp_usbd_builtin_configuration(true);
}
#endif

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    char serial_buf[MICROPY_HW_USB_DESC_STR_MAX + 1]; // Includes terminating NUL byte
    static uint16_t desc_wstr[MICROPY_HW_USB_DESC_STR_MAX + 1]; // Includes prefix uint16_t
    const char *desc_str = NULL;
    uint16_t desc_len;

    #if MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE
    desc_str = mp_usbd_runtime_string_cb(index);
    #endif

    if (index == 0) {
        // String descriptor 0 is special, see USB 2.0 section 9.6.7 String
        //
        // Expect any runtime value in desc_str to be a fully formed descriptor
        if (desc_str == NULL) {
            desc_str = "\x04\x03\x09\x04"; // Descriptor for "English"
        }
        if (desc_str[0] < sizeof(desc_wstr)) {
            memcpy(desc_wstr, desc_str, desc_str[0]);
            return desc_wstr;
        }
        return NULL; // Descriptor length too long (or malformed), stall endpoint
    }

    // Otherwise, generate a "UNICODE" string descriptor from the C string

    if (desc_str == NULL) {
        // Fall back to the "static" string
        switch (index) {
            case USBD_STR_SERIAL:
                mp_usbd_port_get_serial_number(serial_buf);
                desc_str = serial_buf;
                break;
            case USBD_STR_MANUF:
                desc_str = MICROPY_HW_USB_MANUFACTURER_STRING;
                break;
            case USBD_STR_PRODUCT:
                desc_str = MICROPY_HW_USB_PRODUCT_FS_STRING;
                break;
            #if CFG_TUD_CDC
            case USBD_STR_CDC:
                desc_str = MICROPY_HW_USB_CDC_INTERFACE_STRING;
                break;
            #endif
            #if CFG_TUD_MSC
            case USBD_STR_MSC:
                desc_str = MICROPY_HW_USB_MSC_INTERFACE_STRING;
                break;
            #endif
            #if CFG_TUD_VIDEO
            case USBD_STR_UVC:
                desc_str = "ESP-VISION Camera";
                break;
            #endif
            default:
                break;
        }
    }

    if (desc_str == NULL) {
        return NULL; // No string, STALL the endpoint
    }

    // Convert from narrow string to wide string
    desc_len = 2;
    for (int i = 0; i < MICROPY_HW_USB_DESC_STR_MAX && desc_str[i] != 0; i++) {
        desc_wstr[1 + i] = desc_str[i];
        desc_len += 2;
    }

    // first byte is length (including header), second byte is string type
    desc_wstr[0] = (TUSB_DESC_STRING << 8) | desc_len;

    return desc_wstr;
}


#if !MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE

const uint8_t *tud_descriptor_device_cb(void) {
    return (const void *)&mp_usbd_builtin_desc_dev;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return index == 0 ? mp_usbd_builtin_configuration(false) : NULL;
}

#else

// If runtime device support is enabled, descriptor callbacks are implemented in usbd.c

#endif // !MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE

#endif // MICROPY_HW_ENABLE_USBDEV
