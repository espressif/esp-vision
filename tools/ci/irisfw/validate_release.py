# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Independently reopen and validate the final Iris release files before upload."""
import argparse
from pathlib import Path
import stat
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.ci.irisfw.format import (  # noqa: E402
    COMPONENT_LIMIT, LIMIT, MINIMUM_RECOVERY_VERSION, ROOT, SCHEMA, application_partition, board_policy, digest, fields, filename,
    inspect_application, partition_table, release_boards, release_tag,
    require, sha256, source_sha, strict_json, uint, validate_build_record,
)


def validate_bundle(path, policy, tag):
    require(path.stat().st_size <= LIMIT, 'Archive size limit exceeded')
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        require(1 <= len(infos) <= 16, 'Invalid archive member count')
        members = {}
        for info in infos:
            require(info.orig_filename == info.filename, 'Archive member name was truncated')
            filename(info.filename)
            mode = info.external_attr >> 16
            require(info.filename not in members, 'Duplicate archive member')
            require(not info.is_dir() and not info.flag_bits & 1
                    and stat.S_IFMT(mode) in (0, stat.S_IFREG), 'Unsupported archive member type')
            require(info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                    and info.extract_version <= 20 and not info.extra, 'Unsupported ZIP features')
            require(info.file_size <= COMPONENT_LIMIT, 'Archive member size limit exceeded')
            members[info.filename] = info
        require(sum(info.file_size for info in infos) <= LIMIT, 'Expanded archive limit exceeded')
        require('manifest.json' in members and 0 < members['manifest.json'].file_size <= 3072,
                'Missing or oversized manifest')
        manifest = strict_json(archive.read('manifest.json'))
        fields(manifest, ('schema', 'release', 'minimum_recovery_version', 'target', 'target_layout_sha256', 'components'))
        require(manifest['schema'] == SCHEMA and manifest['release'] == release_tag(tag), 'Schema or release mismatch')
        require(manifest['minimum_recovery_version'] == MINIMUM_RECOVERY_VERSION, 'Recovery ABI version mismatch')
        fields(manifest['target'], ('chip_id', 'flash_size'))
        require(uint(manifest['target']['chip_id'], 65535) == policy['chip_id']
                and uint(manifest['target']['flash_size']) == policy['flash_size'], 'Target mismatch')
        digest(manifest['target_layout_sha256'])
        components = manifest['components']
        # No board currently requires a data image. Fail closed if that changes;
        # never ship empty FFAT/NVS or silently omit a newly required resource.
        require(isinstance(components, list) and len(components) == 2, 'Application package needs table and app only')
        used, ids, regions, parts = {'manifest.json'}, set(), [], {}
        for component in components:
            fields(component, ('id', 'kind', 'target_offset', 'size', 'sha256', 'file'))
            identifier = uint(component['id'], 255)
            require(identifier and identifier not in ids, 'Duplicate or zero component ID')
            ids.add(identifier)
            kind = component['kind']
            require(kind in ('partition_table', 'application') and kind not in parts, 'Unsupported or duplicate component kind')
            name = filename(component['file'])
            require(name in members and name not in used, 'Missing or duplicate component member')
            used.add(name)
            offset, size = uint(component['target_offset']), uint(component['size'], COMPONENT_LIMIT)
            require(size > 0 and size == members[name].file_size and offset + size <= policy['flash_size'],
                    'Component size or Flash bounds mismatch')
            require(offset % (65536 if kind == 'application' else 4096) == 0, 'Component alignment mismatch')
            data = archive.read(name)
            require(sha256(data) == digest(component['sha256']), 'Component SHA-256 mismatch')
            regions.append((offset, offset + size))
            parts[kind] = (component, data)
        require(set(members) == used, 'Undeclared archive members')
        regions.sort()
        require(regions[0][1] <= regions[1][0], 'Components overlap')
        table, table_bytes = parts['partition_table']
        require(len(table_bytes) == 4096 and table['sha256'] == manifest['target_layout_sha256'], 'Layout digest or padding mismatch')
        partition_table(table_bytes, policy, table['target_offset'])
        app, app_bytes = parts['application']
        slot = application_partition(policy)
        require(app['target_offset'] == slot[2] and app['size'] <= slot[3],
                'Application exceeds or differs from main_app partition')
        identity = inspect_application(app_bytes, policy['chip_id'])
        require(identity['version'] == tag and identity['project'] == 'esp-vision', 'Application version/project mismatch')
    return dict(schema=1, status='passed', board=policy['board'], release=tag,
                bundle_sha256=sha256(path.read_bytes()), layout_sha256=manifest['target_layout_sha256'],
                application=identity, components=components)


def validate_release(directory, tag, source_commit, pipeline_id, root=ROOT):
    release_tag(tag)
    source_sha(source_commit)
    require(uint(pipeline_id) > 0, 'Pipeline ID is required')
    index = strict_json((directory / 'release.json').read_bytes())
    fields(index, ('schema', 'release', 'source_commit', 'pipeline_id', 'boards', 'files'))
    boards = release_boards(root)
    require(index['schema'] == 'esp-vision/irisfw-release/1' and index['release'] == tag
            and index['source_commit'] == source_commit and index['pipeline_id'] == pipeline_id
            and index['boards'] == boards, 'Release identity or board membership mismatch')
    require(isinstance(index['files'], dict), 'Invalid release files')
    expected = set()
    for board in boards:
        stem = f'esp-vision-{board}-{tag}'
        names = [stem + suffix for suffix in ('.irisfw', '.provenance.json', '.validation.json')]
        expected.update(names)
        report = validate_bundle(directory / names[0], board_policy(board, root), tag)
        require(strict_json((directory / names[2]).read_bytes()) == report, 'Validation report mismatch')
        provenance = strict_json((directory / names[1]).read_bytes())
        fields(provenance, ('schema', 'build', 'bundle_sha256', 'layout_sha256', 'packager_source_commit', 'package_kind'))
        build = provenance['build']
        require(provenance['schema'] == 1 and provenance['package_kind'] == 'mosaico-retained-application'
                and provenance['packager_source_commit'] == source_commit
                and provenance['bundle_sha256'] == report['bundle_sha256']
                and provenance['layout_sha256'] == report['layout_sha256'], 'Provenance mismatch')
        validate_build_record(build, board_policy(board, root), tag, source_commit, pipeline_id)
        app_component = next(c for c in report['components'] if c['kind'] == 'application')
        require(build['application'] == report['application']
                and build['inputs']['application']['sha256'] == app_component['sha256']
                and build['inputs']['application']['size'] == app_component['size']
                and build['layout_sha256'] == report['layout_sha256'], 'Build provenance differs from bundle')
    require(set(index['files']) == expected, 'Release file set mismatch')
    require({p.name for p in directory.iterdir()} == expected | {'SHA256SUMS', 'release.json'}, 'Unexpected release files')
    for name, descriptor in index['files'].items():
        filename(name)
        fields(descriptor, ('sha256', 'size'))
        path = directory / name
        require(path.is_file() and not path.is_symlink(), 'Invalid release file')
        data = path.read_bytes()
        require(len(data) == uint(descriptor['size']) and sha256(data) == digest(descriptor['sha256']), 'Release file hash mismatch')
    checksums = ''.join(f"{index['files'][name]['sha256']}  {name}\n" for name in sorted(expected))
    checksums += f"{sha256((directory / 'release.json').read_bytes())}  release.json\n"
    require((directory / 'SHA256SUMS').read_text() == checksums, 'SHA256SUMS mismatch')
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--pipeline-id', type=int, required=True)
    args = parser.parse_args()
    result = validate_release(args.root, args.tag, args.source_sha, args.pipeline_id)
    print(f"Validated {len(result['boards'])} Iris application packages")


if __name__ == '__main__':
    main()
