# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Capture sanitized build provenance on the firmware runner, before artifacts leave it."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.ci.irisfw.format import (  # noqa: E402
    PROFILE, application_partition, board_policy, inspect_application, json_bytes, partition_table, read_input,
    release_boards, require, settings, sha256, strict_json, validate_build_record,
)


def record_build(build, policy, source_commit, pipeline_id, job_id, idf_image, idf_commit):
    args = strict_json((build / 'flasher_args.json').read_bytes())
    description = strict_json((build / 'project_description.json').read_bytes())
    config_bytes = (build / 'sdkconfig').read_bytes()
    config = settings(config_bytes.decode())
    cache = (build / 'CMakeCache.txt').read_text()
    require('ESP_VISION_ENABLE_IRISFW:BOOL=ON' in cache.splitlines(), 'Iris compile macro is not enabled')
    require(config.get('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE') == 'y'
            and config.get('CONFIG_ESPTOOLPY_FLASHMODE_QIO') == 'y', 'Retained boot/Flash configuration mismatch')
    require(config.get('CONFIG_APP_EXCLUDE_PROJECT_VER_VAR') != 'y'
            and config.get('CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR') != 'y', 'Application identity is disabled')
    require(config['CONFIG_IDF_TARGET'] == '"' + policy['target'] + '"'
            and config['CONFIG_ESPTOOLPY_FLASHSIZE'] == f'"{policy["flash_size"] // (1024 * 1024)}MB"'
            and int(config['CONFIG_PARTITION_TABLE_OFFSET'], 0) == 0x8000, 'Effective sdkconfig target/layout mismatch')
    require(args['extra_esptool_args']['chip'] == policy['target']
            and args['flash_settings']['flash_size'] == f'{policy["flash_size"] // (1024 * 1024)}MB',
            'Flasher target/Flash size mismatch')
    keys = {'application': 'app', 'partition_table': 'partition-table', 'bootloader': 'bootloader'}
    declared = {args[key]['offset']: args[key]['file'] for key in keys.values()}
    ota_data = args['flash_files'].get('0x9000')
    require(ota_data is not None, 'Missing standard blank OTA metadata build input')
    declared['0x9000'] = ota_data
    require(args['flash_files'] == declared, 'Unqualified resource or extra flash input; add an explicit resource policy')
    require(all(str(args[key].get('encrypted', 'false')).lower() == 'false' for key in keys.values()),
            'Encrypted components need a separate policy')
    names = {kind: args[key]['file'] for kind, key in keys.items()}
    names['ota_data'] = ota_data
    names.update(flasher_args='flasher_args.json', project_description='project_description.json')
    inputs = {kind: read_input(build, name) for kind, name in names.items()}
    require(inputs['ota_data'] == b'\xff' * 8192, 'Unexpected OTA bootstrap metadata; never package it')
    table = partition_table(inputs['partition_table'], policy, int(args['partition-table']['offset'], 0))
    application = inspect_application(inputs['application'], policy['chip_id'])
    require(description['project_name'] == application['project'] == 'esp-vision'
            and description['project_version'] == application['version']
            and description['target'] == policy['target'], 'Build description differs from compiled application')
    slot = application_partition(policy)
    require(int(args['app']['offset'], 0) == slot[2]
            and len(inputs['application']) <= slot[3], 'Application exceeds or differs from main_app partition')
    record = dict(schema=1, profile=PROFILE, board=policy['board'], target=policy['target'], source_commit=source_commit,
                  pipeline_id=pipeline_id, job_id=job_id, idf_image=idf_image, idf_commit=idf_commit,
                  application=application, flash_size=policy['flash_size'], sdkconfig_sha256=sha256(config_bytes),
                  layout_sha256=sha256(table),
                  inputs={kind: dict(file=names[kind], size=len(data), sha256=sha256(data)) for kind, data in inputs.items()})
    validate_build_record(record, policy, application['version'], source_commit, pipeline_id)
    (build / 'irisfw-build.json').write_bytes(json_bytes(record))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board')
    args = parser.parse_args()
    if args.board not in release_boards() or os.environ.get('ESP_VISION_ENABLE_IRISFW') != '1':
        print('Iris build is not enabled for this board/job; no Iris package is published')
        return
    policy = board_policy(args.board)
    candidates = list(Path('build', args.board).glob('idf*/flasher_args.json'))
    require(len(candidates) == 1 and candidates[0].parent.name == policy['build_directory'], 'Ambiguous build directory')
    idf_commit = subprocess.check_output(['git', '-C', os.environ['IDF_PATH'], 'rev-parse', 'HEAD'], text=True).strip()
    record_build(candidates[0].parent, policy, os.environ['CI_COMMIT_SHA'], int(os.environ['CI_PIPELINE_ID']),
                 int(os.environ['CI_JOB_ID']), os.environ['IDF_IMAGE'], idf_commit)
    print(f'Recorded Iris release inputs for {args.board}')


if __name__ == '__main__':
    main()
