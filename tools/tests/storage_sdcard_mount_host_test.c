/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef int esp_err_t;
typedef int FATFS;
typedef int esp_vision_storage_state_t;
typedef unsigned char BYTE;
typedef struct {
    unsigned flags;
    int slot;
    esp_err_t (*init)(void);
    union {
        esp_err_t (*deinit)(void);
        esp_err_t (*deinit_p)(int);
    };
} sdmmc_host_t;
typedef struct {
    struct {
        int sector_size;
    } csd;
} sdmmc_card_t;
typedef struct {
    int width;
} sdmmc_slot_config_t;
typedef struct {
    const char *base_path, *fat_drive;
    int max_files;
} esp_vfs_fat_conf_t;
typedef struct {
    int fatfs_result, state;
} esp_vision_storage_status_t;

#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NOT_SUPPORTED 4
#define FR_OK 0
#define ESP_VISION_STORAGE_UNAVAILABLE 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define FF_DRV_NOT_USED 255
#define ESP_VISION_SDCARD_SLOT 1
#define ESP_VISION_SDCARD_BUS_WIDTH 4
#define ESP_VISION_SDCARD_MOUNT_PATH "/sdcard"
#define ESP_VISION_STORAGE_BLOCK_SIZE 512
#define ESP_VISION_STORAGE_MAX_FILES 16
#define SDMMC_HOST_FLAG_DEINIT_ARG 1
#define ESP_IDF_VERSION_VAL(a, b, c) ((a) * 10000 + (b) * 100 + (c))
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

static bool s_sdcard_mounted, s_sdcard_host_initialized;
static sdmmc_host_t s_sdcard_host;
static sdmmc_card_t *s_sdcard, s_sdcard_info;
static FATFS *s_sdcard_fs, filesystem;
static BYTE s_sdcard_pdrv;
static char s_sdcard_drive[3];
static uint8_t s_sdcard_mkfs_work[4096];
static int s_sdcard_fatfs_result;
static int s_status_mux;
static esp_vision_storage_state_t s_sdcard_mount_state;
static unsigned failing_stage, stage_count, deinit_calls, disk_unregisters, vfs_unregisters, format_calls;
static bool fail_deinit, fail_unmount, fail_vfs_unregister, present, automatic_seen;

static esp_err_t next_stage(void)
{
    return ++stage_count == failing_stage ? ESP_FAIL : ESP_OK;
}
static esp_err_t host_init(void)
{
    return next_stage();
}
static esp_err_t host_deinit(int slot)
{
    assert(slot == ESP_VISION_SDCARD_SLOT);
    deinit_calls++;
    return fail_deinit ? ESP_FAIL : ESP_OK;
}
#define SDMMC_HOST_DEFAULT() { .flags = SDMMC_HOST_FLAG_DEINIT_ARG, .slot = 0, .init = host_init, .deinit_p = host_deinit }
#define SDMMC_SLOT_CONFIG_DEFAULT() { .width = 1 }

static void esp_vision_sdcard_init0(void) {}
static bool esp_vision_sdcard_is_present(void)
{
    return present;
}
static esp_err_t esp_vision_sdcard_preinit_host(sdmmc_host_t *host, int slot)
{
    assert(host->slot == slot);
    return ESP_OK;
}
static void esp_vision_sdcard_deinit_host(sdmmc_host_t *host, int slot)
{
    assert(host->slot == slot);
}
static esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config)
{
    assert(slot == ESP_VISION_SDCARD_SLOT && config->width == 4);
    return next_stage();
}
static esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    assert(host->slot == ESP_VISION_SDCARD_SLOT);
    card->csd.sector_size = 512;
    return next_stage();
}
static esp_err_t ff_diskio_get_drive(BYTE *drive)
{
    esp_err_t ret = next_stage();
    if (ret == ESP_OK) {
        *drive = 1;
    }
    return ret;
}
static void esp_vision_storage_register_sdcard_diskio(void)
{
    assert(s_sdcard != NULL && s_sdcard_pdrv == 1 && !s_sdcard_mounted);
}
static void ff_diskio_unregister(BYTE drive)
{
    assert(drive == 1);
    disk_unregisters++;
}
static esp_err_t register_vfs(const esp_vfs_fat_conf_t *conf, FATFS **fs)
{
    assert(strcmp(conf->base_path, "/sdcard") == 0 && strcmp(conf->fat_drive, "1:") == 0);
    esp_err_t ret = next_stage();
    if (ret == ESP_OK) {
        *fs = &filesystem;
    }
    return ret;
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define esp_vfs_fat_register register_vfs
#else
#define esp_vfs_fat_register_cfg register_vfs
#endif
static esp_err_t esp_vfs_fat_unregister_path(const char *path)
{
    assert(strcmp(path, "/sdcard") == 0);
    vfs_unregisters++;
    return fail_vfs_unregister ? ESP_FAIL : ESP_OK;
}
static int f_mount(FATFS *fs, const char *drive, int immediate)
{
    assert(fs == NULL && strcmp(drive, "1:") == 0 && immediate == 0);
    return fail_unmount ? 1 : FR_OK;
}
static esp_err_t esp_vision_storage_mount_sd_fat(FATFS *fs, const char *drive, void *work, unsigned size,
                                                 bool automatic, esp_vision_storage_status_t *status)
{
    assert(fs == &filesystem && strcmp(drive, "1:") == 0 && work == s_sdcard_mkfs_work && size == 4096);
    automatic_seen = automatic;
    format_calls++;
    status->fatfs_result = 13;
    status->state = 1;
    return next_stage();
}

/* PRODUCTION_FUNCTIONS */

static void reset(void)
{
    s_sdcard_mounted = s_sdcard_host_initialized = false;
    s_sdcard = NULL;
    s_sdcard_fs = NULL;
    s_sdcard_pdrv = FF_DRV_NOT_USED;
    failing_stage = stage_count = deinit_calls = disk_unregisters = vfs_unregisters = format_calls = 0;
    fail_deinit = fail_unmount = fail_vfs_unregister = automatic_seen = false;
    present = true;
}

int main(void)
{
    reset();
    assert(esp_vision_storage_mount_sdcard(true) == ESP_OK && automatic_seen && s_sdcard_mounted);
    assert(format_calls == 1 && s_sdcard_fatfs_result == 13);
    assert(esp_vision_storage_unregister_sdcard() == ESP_OK);
    assert(!s_sdcard_mounted && !s_sdcard_host_initialized && !s_sdcard_fs && !s_sdcard);
    assert(deinit_calls == 1 && disk_unregisters == 1 && vfs_unregisters == 1);
    for (unsigned stage = 1; stage <= 6; ++stage) {
        reset();
        failing_stage = stage;
        assert(esp_vision_storage_mount_sdcard(false) != ESP_OK && !s_sdcard_mounted && !automatic_seen);
        assert(!s_sdcard_host_initialized && s_sdcard_pdrv == FF_DRV_NOT_USED && !s_sdcard_fs && !s_sdcard);
        assert(deinit_calls == (stage > 1));
        assert(disk_unregisters == (stage > 4) && vfs_unregisters == (stage > 5));
    }
    reset();
    present = false;
    assert(esp_vision_storage_mount_sdcard(true) == ESP_ERR_NOT_FOUND && stage_count == 0);
    reset();
    failing_stage = 3;
    fail_deinit = true;
    assert(esp_vision_storage_mount_sdcard(true) != ESP_OK && s_sdcard_host_initialized);
    assert(esp_vision_storage_mount_sdcard(true) == ESP_ERR_INVALID_STATE);
    reset();
    assert(esp_vision_storage_mount_sdcard(false) == ESP_OK && !automatic_seen);
    fail_unmount = true;
    assert(esp_vision_storage_unregister_sdcard() != ESP_OK && !s_sdcard_mounted && s_sdcard_fs != NULL);
    assert(deinit_calls == 0 && disk_unregisters == 0);
    assert(esp_vision_storage_mount_sdcard(true) == ESP_ERR_INVALID_STATE);
    return 0;
}
