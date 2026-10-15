/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_VISION_TEST_TUSB_H
#define ESP_VISION_TEST_TUSB_H

#include <stdint.h>

#define SCSI_SENSE_NOT_READY 0x02
#define SCSI_SENSE_ILLEGAL_REQUEST 0x05
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1e

void tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t additional_sense_code,
                       uint8_t additional_sense_code_qualifier);
#endif
