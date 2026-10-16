/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "device_identity.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "boardconfig.h"
#include "firmware_info.h"
#include "esp_mac.h"

static char s_device_id[ESP_VISION_DEVICE_ID_LENGTH + 1];
static char s_hostname[ESP_VISION_HOSTNAME_MAX + 1];

/* DNS labels: ASCII lower case, digits and single hyphens, no edge hyphens. */
static size_t dns_label(char *out, size_t capacity, const char *source)
{
    size_t n = 0;
    for (; *source != '\0'; ++source) {
        unsigned char c = (unsigned char) * source;
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (n + 1 >= capacity) {
                return 0;
            }
            out[n++] = c;
        } else if (n > 0 && out[n - 1] != '-') {
            if (n + 1 >= capacity) {
                return 0;
            }
            out[n++] = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') {
        --n;
    }
    out[n] = '\0';
    return n;
}

bool esp_vision_identity_init(void)
{
    if (s_hostname[0] != '\0') {
        return true;
    }
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return false;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char board[64];
    char version[128];
    if (!dns_label(board, sizeof(board), ESP_VISION_BOARD_TYPE)) {
        return false;
    }
    /* Keep the full board name; long build versions get a deterministic digest. */
    size_t prefix_length = strlen("esp-vision-") + strlen(board) + 1 + 6 + 1;
    if (prefix_length + 9 > ESP_VISION_HOSTNAME_MAX) {
        return false;
    }
    size_t budget = ESP_VISION_HOSTNAME_MAX - prefix_length;
    size_t version_length = dns_label(version, sizeof(version), ESP_VISION_FIRMWARE_VERSION);
    if (!version_length || version_length > budget) {
        uint32_t hash = 2166136261u;
        for (const unsigned char *p = (const unsigned char *)ESP_VISION_FIRMWARE_VERSION; *p; ++p) {
            hash = (hash ^ *p) * 16777619u;
        }
        if (!version_length) {
            strcpy(version, "build");
        }
        size_t keep = budget - 9;
        if (strlen(version) < keep) {
            keep = strlen(version);
        }
        while (keep > 0 && version[keep - 1] == '-') {
            --keep;
        }
        snprintf(version + keep, sizeof(version) - keep, "-%08lx", (unsigned long)hash);
    }
    int written = snprintf(s_hostname, sizeof(s_hostname), "esp-vision-%s-%s-%s",
                           board, s_device_id + 6, version);
    if (written <= 0 || written > ESP_VISION_HOSTNAME_MAX) {
        s_hostname[0] = '\0';
        return false;
    }
    return true;
}

const char *esp_vision_device_id(void)
{
    return s_device_id;
}

const char *esp_vision_hostname(void)
{
    return s_hostname;
}
