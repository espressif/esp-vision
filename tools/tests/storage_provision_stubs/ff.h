/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_VISION_TEST_FAT_H
#define ESP_VISION_TEST_FAT_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef char TCHAR;
typedef uint32_t DWORD;
typedef struct {
    int mounted;
} FATFS;
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR = 1,
    FR_INT_ERR = 2,
    FR_NOT_READY = 3,
    FR_NO_FILESYSTEM = 13,
} FRESULT;
typedef struct {
    BYTE fmt;
    BYTE n_fat;
    UINT align;
    UINT n_root;
    uint32_t au_size;
} MKFS_PARM;
#define FM_FAT 0x01
#define FM_ANY 0x07
#define FM_SFD 0x08
FRESULT f_mount(FATFS *filesystem, const char *drive, BYTE immediate);
FRESULT f_mkfs(const char *drive, const MKFS_PARM *options, void *work, UINT size);
FRESULT f_getlabel(const TCHAR *drive, TCHAR *label, DWORD *serial);
FRESULT f_setlabel(const TCHAR *label);
#endif
