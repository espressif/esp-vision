/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "device_identity.h"
#include "ev_launcher_state.h"

static const uint8_t factory_mac[6] = {
#if TEST_IEEE802154
    0x30, 0xed, 0xa0, 0xed, 0xa1, 0xb0
#else
    0xe8, 0xf6, 0x0a, 0xe7, 0x56, 0x42
#endif
};
static const uint8_t custom_base[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static int read_error;
static unsigned int factory_reads;
static unsigned int default_reads;
int ev_test_in_critical;

/* Match the IDF's two different contracts, including the 8-byte legacy write. */
esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    default_reads++;
    memcpy(mac, factory_mac, 6);
#if TEST_IEEE802154
    memmove(mac + 5, mac + 3, 3);
    mac[3] = 0xff;
    mac[4] = 0xfe;
#endif
    return ESP_OK;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (type == ESP_MAC_BASE) {
        memcpy(mac, custom_base, 6);
        return ESP_OK;
    }
    assert(type == ESP_MAC_EFUSE_FACTORY);
    factory_reads++;
    if (read_error) {
        return read_error;
    }
    memcpy(mac, factory_mac, 6);
    return ESP_OK;
}

uint32_t esp_random(void)
{
    static uint32_t value;
    return ++value;
}

uint32_t ev_session_vm_epoch(void)
{
    assert(!ev_test_in_critical);
    return 7;
}

typedef struct {
    size_t length;
    uint8_t bytes[6];
} test_bytes_t;
typedef test_bytes_t *mp_obj_t;
static test_bytes_t returned_bytes;
static jmp_buf raise_target;
static int raised_errno;
#define MP_EIO (5)
#define MICROPY_HW_USB_DESC_STR_MAX (32)
#define MP_STATIC_ASSERT(condition) _Static_assert(condition, #condition)

static mp_obj_t mp_obj_new_bytes(const uint8_t *bytes, size_t length)
{
    assert(length == 6);
    returned_bytes.length = length;
    memcpy(returned_bytes.bytes, bytes, length);
    return &returned_bytes;
}

_Noreturn static void mp_raise_OSError(int value)
{
    raised_errno = value;
    longjmp(raise_target, 1);
}

static void mp_usbd_hex_str(char *out, const uint8_t *bytes, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    assert(length == 6);
    for (size_t index = 0; index < length; index++) {
        out[index * 2] = digits[bytes[index] >> 4];
        out[index * 2 + 1] = digits[bytes[index] & 15];
    }
    out[length * 2] = '\0';
}

/* The test runner extracts these two functions verbatim, with their MIT heads. */
#include "factory_mac_callers.inc"

static const char *expected_identity(void)
{
#if TEST_IEEE802154
    return "30eda0eda1b0";
#else
    return "e8f60ae75642";
#endif
}

static void assert_canaries(const uint8_t *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++) {
        assert(bytes[index] == 0xa5);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "contract-canaries") == 0) {
        uint8_t storage[18];
        memset(storage, 0xa5, sizeof(storage));
        assert(esp_efuse_mac_get_default(storage + 4) == ESP_OK);
        assert_canaries(storage, 4);
#if TEST_IEEE802154
        assert(storage[10] == factory_mac[4] && storage[11] == factory_mac[5]);
        assert(memcmp(storage + 4, factory_mac, 6) != 0);
        assert_canaries(storage + 12, 6);
#else
        assert(memcmp(storage + 4, factory_mac, 6) == 0);
        assert_canaries(storage + 10, 8);
#endif
        memset(storage, 0xa5, sizeof(storage));
        assert(esp_read_mac(storage + 4, ESP_MAC_EFUSE_FACTORY) == ESP_OK);
        assert(memcmp(storage + 4, factory_mac, 6) == 0);
        assert_canaries(storage, 4);
        assert_canaries(storage + 10, 8);
        return 0;
    }
    if (strcmp(argv[1], "machine") == 0) {
        mp_obj_t result = mp_machine_unique_id();
        assert(result->length == 6 && memcmp(result->bytes, factory_mac, 6) == 0);
    } else if (strcmp(argv[1], "usb") == 0) {
        struct {
            uint8_t before[8];
            char serial[13];
            uint8_t after[8];
        } guarded;
        memset(&guarded, 0xa5, sizeof(guarded));
        mp_usbd_port_get_serial_number(guarded.serial);
        assert(strcmp(guarded.serial, expected_identity()) == 0);
        assert_canaries(guarded.before, sizeof(guarded.before));
        assert_canaries(guarded.after, sizeof(guarded.after));
    } else if (strcmp(argv[1], "identity") == 0) {
        assert(esp_vision_identity_init());
        assert(strcmp(esp_vision_device_id(), expected_identity()) == 0);
        assert(strstr(esp_vision_hostname(), expected_identity() + 6) != NULL);
    } else if (strcmp(argv[1], "launcher") == 0 || strcmp(argv[1], "launcher-error") == 0) {
        read_error = strcmp(argv[1], "launcher-error") == 0;
        ev_launcher_state_snapshot_t result;
        esp_vision_launcher_state_init0();
        assert(esp_vision_launcher_state_snapshot(&result));
        assert(result.device_id_valid == !read_error);
        assert(strcmp(result.device_id, read_error ? "" : expected_identity()) == 0);
    } else if (strcmp(argv[1], "read-errors") == 0) {
        read_error = 1;
        assert(!esp_vision_identity_init());
        assert(esp_vision_device_id()[0] == '\0');
        if (setjmp(raise_target) == 0) {
            (void)mp_machine_unique_id();
            assert(!"failed factory read must raise OSError");
        }
        assert(raised_errno == MP_EIO);
        char serial[16];
        memset(serial, 0xa5, sizeof(serial));
        mp_usbd_port_get_serial_number(serial);
        assert(serial[0] == '\0');
        assert_canaries((const uint8_t *)serial + 1, sizeof(serial) - 1);
        read_error = 0;
        assert(esp_vision_identity_init());
        assert(strcmp(esp_vision_device_id(), expected_identity()) == 0);
    } else {
        assert(!"unknown test case");
    }
    assert(factory_reads > 0);
    assert(default_reads == 0);
    return 0;
}
