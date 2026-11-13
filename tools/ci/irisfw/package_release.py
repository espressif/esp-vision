# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Build deterministic Iris application packages from one qualified CI pipeline."""
import argparse
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.ci.irisfw.format import (  # noqa: E402
    MINIMUM_RECOVERY_VERSION, ROOT, SCHEMA, board_policy, inspect_application, json_bytes,
    partition_table, read_input, release_boards, release_tag, require, sha256,
    source_sha, strict_json, uint, validate_build_record,
)
from tools.ci.irisfw.validate_release import validate_bundle, validate_release  # noqa: E402


def package_board(build, output, policy, tag, source_commit, pipeline_id):
    record = strict_json((build / 'irisfw-build.json').read_bytes())
    validate_build_record(record, policy, tag, source_commit, pipeline_id)
    inputs = {}
    for kind, descriptor in record['inputs'].items():
        data = read_input(build, descriptor['file'])
        require(len(data) == descriptor['size'] and sha256(data) == descriptor['sha256'], 'Build input hash mismatch')
        inputs[kind] = data
    identity = strict_json((build / 'release-build-identity.json').read_bytes())
    require(identity['board'] == policy['board'] and identity['sourceCommit'] == source_commit
            and identity['firmwareVersion'] == tag and identity['appSha256'] == sha256(inputs['application']),
            'Release build identity mismatch')
    args = strict_json(inputs['flasher_args'])
    table = partition_table(inputs['partition_table'], policy, int(args['partition-table']['offset'], 0))
    require(sha256(table) == record['layout_sha256'], 'Build layout mismatch')
    require(inspect_application(inputs['application'], policy['chip_id']) == record['application'],
            'Build application identity mismatch')
    payloads = {'partition-table.bin': table, 'app.bin': inputs['application']}
    components = []
    for identifier, kind, key, name in [(1, 'partition_table', 'partition-table', 'partition-table.bin'),
                                        (2, 'application', 'app', 'app.bin')]:
        data = payloads[name]
        components.append(dict(id=identifier, kind=kind, target_offset=int(args[key]['offset'], 0),
                               size=len(data), sha256=sha256(data), file=name))
    manifest = dict(schema=SCHEMA, release=tag, minimum_recovery_version=MINIMUM_RECOVERY_VERSION,
                    target=dict(chip_id=policy['chip_id'], flash_size=policy['flash_size']),
                    target_layout_sha256=sha256(table), components=components)
    stem = f"esp-vision-{policy['board']}-{tag}"
    path = output / (stem + '.irisfw')
    # STORED makes package bytes independent of zlib/platform versions.
    with zipfile.ZipFile(path, 'x', compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name, data in [('manifest.json', json_bytes(manifest)), *payloads.items()]:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    report = validate_bundle(path, policy, tag)
    provenance = dict(schema=1, build=record, bundle_sha256=report['bundle_sha256'],
                      layout_sha256=report['layout_sha256'], packager_source_commit=source_commit,
                      package_kind='mosaico-retained-application')
    for suffix, value in [('.provenance.json', provenance), ('.validation.json', report)]:
        with (output / (stem + suffix)).open('xb') as stream:
            stream.write(json_bytes(value))


def package_release(build_root, output, tag, source_commit, pipeline_id, root=ROOT):
    release_tag(tag)
    source_sha(source_commit)
    require(uint(pipeline_id) > 0, 'Pipeline ID required')
    require(not output.exists(), 'Output must be new; preserve failed/frozen candidates')
    boards = release_boards(root)
    builds = []
    for board in boards:
        policy = board_policy(board, root)
        candidates = list((build_root / board).glob('idf*/flasher_args.json'))
        expected = build_root / board / policy['build_directory']
        require(len(candidates) == 1 and candidates[0].parent == expected, f'{board}: missing or ambiguous IDF build')
        builds.append((expected, policy))
    output.mkdir(parents=True)
    for build, policy in builds:
        package_board(build, output, policy, tag, source_commit, pipeline_id)
    files = {path.name: dict(size=path.stat().st_size, sha256=sha256(path.read_bytes()))
             for path in sorted(output.iterdir())}
    index = dict(schema='esp-vision/irisfw-release/1', release=tag, source_commit=source_commit,
                 pipeline_id=pipeline_id, boards=boards, files=files)
    (output / 'release.json').write_bytes(json_bytes(index))
    checksums = ''.join(f"{files[name]['sha256']}  {name}\n" for name in sorted(files))
    checksums += f"{sha256(json_bytes(index))}  release.json\n"
    (output / 'SHA256SUMS').write_text(checksums)
    validate_release(output, tag, source_commit, pipeline_id, root)
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--pipeline-id', type=int, required=True)
    args = parser.parse_args()
    result = package_release(args.build_root, args.output, args.tag, args.source_sha, args.pipeline_id)
    print(f"Packaged {len(result['boards'])} boards without rebuilding firmware")


if __name__ == '__main__':
    main()
