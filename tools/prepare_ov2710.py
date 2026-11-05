# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Prepare a checked Apache-2.0 OV2710 source copy; keep the component pristine."""
import argparse
import hashlib
from pathlib import Path

SOURCE_SHA256 = '4a9af395151527b156b1d685c39b298502490f33a8391a641a51ac72b3bc8876'


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('Unsupported OV2710 lifecycle source; review the adaptation')
    return source.replace(before, after, 1)


def prepare(source):
    if hashlib.sha256(source.encode()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Unsupported esp_cam_sensor 2.4.0 ov2710.c; expected ' + SOURCE_SHA256)
    source = replace_once(source, '#include "freertos/timers.h"',
                          '#include "freertos/timers.h"\n#include "freertos/semphr.h"\n#include "ov2710_lifecycle.h"')
    source = replace_once(source, '    TimerHandle_t wb_timer_handle;',
                          '    TimerHandle_t wb_timer_handle;\n'
                          '    StaticSemaphore_t wb_barrier_storage;\n'
                          '    SemaphoreHandle_t wb_barrier;\n'
                          '    bool wb_barrier_pending;\n'
                          '    bool wb_delete_started;\n'
                          '    bool wb_delete_complete;')
    source = replace_once(source, 'static void wb_timer_callback(TimerHandle_t timer)',
                          '#include "ov2710_lifecycle.inc"\n\nstatic void wb_timer_callback(TimerHandle_t timer)')
    source = replace_once(source, '    if (enable) {\n        ret = ov2710_write',
                          '#if CONFIG_CAMERA_OV2710_STATS_UPDATE_EN\n'
                          '    if (enable && cam_ov2710->wb_delete_started) {\n'
                          '        return ESP_ERR_INVALID_STATE;\n'
                          '    }\n#endif\n'
                          '    if (enable) {\n        ret = ov2710_write')
    source = replace_once(source, '        if (pdTRUE != xTimerStop(cam_ov2710->wb_timer_handle, 10)) {',
                          '        if (cam_ov2710->wb_timer_handle != NULL &&\n'
                          '            pdTRUE != xTimerStop(cam_ov2710->wb_timer_handle, 10)) {')
    source = replace_once(source, '            ESP_LOGE(TAG, "Timer start err");',
                          '            ESP_LOGE(TAG, "Timer start err");\n            return ESP_ERR_TIMEOUT;')
    source = replace_once(source, '            ESP_LOGE(TAG, "Timer stop err");',
                          '            ESP_LOGE(TAG, "Timer stop err");\n            return ESP_ERR_TIMEOUT;')
    source = replace_once(source, '            xTimerDelete(cam_ov2710->wb_timer_handle, portMAX_DELAY);',
                          '            esp_err_t ret = esp_vision_ov2710_prepare_delete(dev);\n'
                          '            if (ret != ESP_OK) {\n                return ret;\n            }')
    source = replace_once(source,
                          'static esp_err_t ov2710_delete(esp_cam_sensor_device_t *dev)\n{\n'
                          '    struct ov2710_cam *cam_ov2710 = (struct ov2710_cam *)dev->priv;\n',
                          'static esp_err_t ov2710_delete(esp_cam_sensor_device_t *dev)\n{\n')
    source = replace_once(source, '    // Init cam privae pata',
                          '    cam_ov2710->wb_barrier = xSemaphoreCreateBinaryStatic(&cam_ov2710->wb_barrier_storage);\n'
                          '    // Init cam privae pata')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(prepare(args.source.read_text()))


if __name__ == '__main__':
    main()
