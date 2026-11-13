# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise release input, binary format, archive and immutable-upload failures."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from urllib.request import Request
import zipfile

import yaml

from tools.ci.irisfw import format as fmt
from tools.ci.irisfw.package_release import package_release
from tools.ci.irisfw.publish_release import DownloadRedirects, publish_files
from tools.ci.irisfw.record_build import record_build
from tools.ci.irisfw.validate_release import validate_bundle, validate_release

TAG = '2026.11.12'
SOURCE = 'a' * 40
PIPELINE = 123


def application(chip, tag=TAG, idf='v5.5.6'):
    header = bytearray(24)
    header[:2] = b'\xe9\x01'
    struct.pack_into('<H', header, 12, chip)
    header[23] = 1
    segment = bytearray(256)
    struct.pack_into('<I', segment, 0, 0xabcd5432)
    segment[16:16 + len(tag)] = tag.encode()
    segment[48:58] = b'esp-vision'
    segment[112:112 + len(idf)] = idf.encode()
    segment[144:176] = b'\xab' * 32
    checksum = 0xef
    for byte in segment:
        checksum ^= byte
    raw = header + struct.pack('<II', 0x3c000020, len(segment)) + segment
    raw += b'\0' * (15 - len(raw) % 16) + bytes([checksum])
    return bytes(raw) + hashlib.sha256(raw).digest()


def table_image(policy):
    data = b''.join(struct.pack('<HBBII16sI', 0x50aa, kind, subtype, offset, size, name.encode(), flags)
                    for kind, subtype, offset, size, name, flags in policy['partitions'])
    data += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(data).digest()
    return data.ljust(3072, b'\xff')


def make_build(root, board, tag=TAG):
    policy = fmt.board_policy(board)
    build = root / board / policy['build_directory']
    build.mkdir(parents=True)
    (build / 'partition_table').mkdir()
    (build / 'bootloader').mkdir()
    image = application(policy['chip_id'], tag, 'v6.1-dev-123' if policy['target'] == 'esp32s31' else 'v5.5.6')
    (build / 'renamed-project.bin').write_bytes(image)
    (build / 'partition_table/partition-table.bin').write_bytes(table_image(policy))
    (build / 'bootloader/bootloader.bin').write_bytes(b'boot')
    boot_offset = '0x0' if policy['target'] == 'esp32s3' else '0x2000'
    args = dict(app=dict(file='renamed-project.bin', offset='0x210000', encrypted='false'),
                bootloader=dict(file='bootloader/bootloader.bin', offset=boot_offset, encrypted='false'),
                **{'partition-table': dict(file='partition_table/partition-table.bin', offset='0x8000', encrypted='false')})
    args['flash_files'] = {args[key]['offset']: args[key]['file'] for key in ('app', 'bootloader', 'partition-table')}
    args['flash_files']['0x9000'] = 'ota_data_initial.bin'
    (build / 'ota_data_initial.bin').write_bytes(b'\xff' * 8192)
    args['extra_esptool_args'] = {'chip': policy['target']}
    size = f'{policy["flash_size"] // (1024 * 1024)}MB'
    args['flash_settings'] = {'flash_size': size}
    (build / 'flasher_args.json').write_bytes(fmt.json_bytes(args))
    (build / 'project_description.json').write_bytes(fmt.json_bytes(dict(
        project_name='esp-vision', project_version=tag, target=policy['target'])))
    (build / 'sdkconfig').write_text(f'CONFIG_IDF_TARGET="{policy["target"]}"\n'
                                    f'CONFIG_ESPTOOLPY_FLASHSIZE="{size}"\nCONFIG_PARTITION_TABLE_OFFSET=0x8000\n'
                                    'CONFIG_ESPTOOLPY_FLASHMODE_QIO=y\nCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n')
    (build / 'CMakeCache.txt').write_text('ESP_VISION_ENABLE_IRISFW:BOOL=ON\n')
    (build / 'release-build-identity.json').write_bytes(fmt.json_bytes(dict(
        board=board, sourceCommit=SOURCE, firmwareVersion=tag, appSha256=fmt.sha256(image))))
    record_build(build, policy, SOURCE, PIPELINE, 456, policy['idf_image'], 'b' * 40)
    return build


class IrisReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.builds = self.root / 'build'
        self.boards = fmt.release_boards()
        self.board = self.boards[0]
        self.policy = fmt.board_policy(self.board)
        for board in self.boards:
            make_build(self.builds, board)
        self.output = self.root / 'release'

    def package(self, output=None):
        return package_release(self.builds, output or self.output, TAG, SOURCE, PIPELINE)

    def bundle(self):
        return self.output / f'esp-vision-{self.board}-{TAG}.irisfw'

    def rewrite(self, mutate):
        with zipfile.ZipFile(self.bundle()) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        manifest = json.loads(members['manifest.json'])
        mutate(manifest, members)
        members['manifest.json'] = fmt.json_bytes(manifest)
        with zipfile.ZipFile(self.bundle(), 'w') as archive:
            for name, data in members.items():
                archive.writestr(name, data)

    def test_all_release_boards_are_reproducible_and_independently_validated(self):
        index = self.package()
        other = self.root / 'second'
        self.package(other)
        self.assertEqual(index['boards'], ['ESP32_S31_MOSAICO'])
        for path in self.output.iterdir():
            self.assertEqual(path.read_bytes(), (other / path.name).read_bytes())
        self.assertEqual(validate_release(self.output, TAG, SOURCE, PIPELINE), index)
        with zipfile.ZipFile(self.bundle()) as archive:
            self.assertEqual(set(archive.namelist()), {'manifest.json', 'app.bin', 'partition-table.bin'})
            self.assertEqual(len(archive.read('partition-table.bin')), 4096)
            self.assertEqual(archive.read('partition-table.bin')[-1024:], b'\xff' * 1024)
        text = (self.output / f'esp-vision-{self.board}-{TAG}.provenance.json').read_text()
        self.assertNotIn(str(self.root), text)
        self.assertNotIn('esp-mosaico/v2', text)

    def test_tag_validation_rejects_truncation_paths_and_sdk_tags(self):
        for tag in ['../../bad', '2026.11.12/extra', 'device-core-v1.0', 'v1.2\n', 'v1.2-' + 'x' * 27, '版本1']:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                fmt.release_tag(tag)
        self.assertEqual(fmt.release_tag('v1.2-' + 'x' * 26), 'v1.2-' + 'x' * 26)

    def test_missing_and_ambiguous_builds_fail_before_output(self):
        build = self.builds / self.board / self.policy['build_directory']
        args = build / 'flasher_args.json'
        original = args.read_bytes()
        args.unlink()
        with self.assertRaisesRegex(ValueError, 'missing or ambiguous'):
            self.package()
        self.assertFalse(self.output.exists())
        args.write_bytes(original)
        extra = self.builds / self.board / 'idf6.0'
        extra.mkdir()
        (extra / 'flasher_args.json').write_bytes(original)
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            self.package()

    def test_stale_source_pipeline_wrong_board_flash_and_idf_fail(self):
        record_path = self.builds / self.board / self.policy['build_directory'] / 'irisfw-build.json'
        original = json.loads(record_path.read_text())
        for key, value in [('source_commit', 'c' * 40), ('pipeline_id', 999), ('board', 'ESP32_S31_KORVO'),
                           ('flash_size', 8 * 1024 * 1024), ('idf_image', 'release-v6.0')]:
            changed = dict(original, **{key: value})
            record_path.write_bytes(fmt.json_bytes(changed))
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.package(self.root / ('bad-' + key))

    def test_unqualified_resources_and_escaping_inputs_fail(self):
        build = self.builds / self.board / self.policy['build_directory']
        args_path = build / 'flasher_args.json'
        original = json.loads(args_path.read_text())
        for mutated in [dict(original, flash_files={**original['flash_files'], '0x710000': 'models.bin'}),
                        copy.deepcopy(original)]:
            if mutated['flash_files'] == original['flash_files']:
                mutated['app']['file'] = '../../../outside.bin'
                mutated['flash_files']['0x210000'] = '../../../outside.bin'
            args_path.write_bytes(fmt.json_bytes(mutated))
            with self.assertRaises(ValueError):
                record_build(build, self.policy, SOURCE, PIPELINE, 456, self.policy['idf_image'], 'b' * 40)

    def test_tampered_manifest_and_components_fail(self):
        self.package()
        original = self.bundle().read_bytes()
        mutations = [
            lambda m, f: m.update(board_id=self.board),
            lambda m, f: m['target'].update(chip_id=9),
            lambda m, f: m['target'].update(flash_size=8 * 1024 * 1024),
            lambda m, f: m['components'][1].update(id=1),
            lambda m, f: m['components'][1].update(kind='bootloader'),
            lambda m, f: m['components'][1].update(target_offset=0x8000),
            lambda m, f: m['components'][1].update(target_offset=0x1000000),
            lambda m, f: m['components'][1].update(target_offset=0x710000),
            lambda m, f: m['components'][1].update(file='../app.bin'),
            lambda m, f: m.update(target_layout_sha256='0' * 64),
            lambda m, f: f.update({'unexpected.bin': b'undeclared'}),
            lambda m, f: f.update({'app.bin': f['app.bin'][:-1] + b'X'}),
            lambda m, f: m['components'][0].update(size=3072),
            lambda m, f: m['components'][1].update(id=True),
        ]
        for index, mutate in enumerate(mutations):
            self.bundle().write_bytes(original)
            self.rewrite(mutate)
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate_bundle(self.bundle(), self.policy, TAG)

    def test_archive_special_members_duplicate_json_and_bounds_fail(self):
        self.package()
        with zipfile.ZipFile(self.bundle()) as archive:
            original = {name: archive.read(name) for name in archive.namelist()}
        for attack in ['symlink', 'directory', 'duplicate', 'duplicate-json', 'compression', 'large-manifest', 'expanded-limit']:
            with zipfile.ZipFile(self.bundle(), 'w') as archive:
                for name, data in original.items():
                    if name == 'manifest.json' and attack == 'duplicate-json':
                        data = data.replace(b'{', b'{"schema":"bad",', 1)
                    if name == 'manifest.json' and attack == 'large-manifest':
                        data += b' ' * 3072
                    info = zipfile.ZipInfo(name)
                    if name == 'app.bin' and attack == 'symlink':
                        info.create_system = 3
                        info.external_attr = 0o120777 << 16
                    if attack == 'compression':
                        info.compress_type = zipfile.ZIP_BZIP2
                    archive.writestr(info, data)
                if attack == 'directory':
                    archive.writestr('extra/', b'')
                if attack == 'duplicate':
                    import warnings
                    with warnings.catch_warnings():
                        warnings.simplefilter('ignore', UserWarning)
                        archive.writestr('app.bin', b'duplicate')
                if attack == 'expanded-limit':
                    for n in range(3):
                        archive.writestr(f'large{n}.bin', b'\0' * (12 * 1024 * 1024), compress_type=zipfile.ZIP_DEFLATED)
            with self.subTest(attack=attack), self.assertRaises(ValueError):
                validate_bundle(self.bundle(), self.policy, TAG)

    def test_image_and_table_integrity_checks_are_independent_of_zip_hash(self):
        raw = bytearray(application(18))
        raw[250] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            fmt.inspect_application(raw, 18)
        raw = bytearray(application(18))
        raw[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            fmt.inspect_application(raw, 18)
        raw = bytearray(table_image(self.policy))
        raw[20] ^= 1
        with self.assertRaisesRegex(ValueError, 'MD5'):
            fmt.partition_table(raw, self.policy)

    def test_json_must_be_utf8_and_zip_names_must_not_be_nul_truncated(self):
        with self.assertRaises(ValueError):
            fmt.strict_json('{"schema":1}'.encode('utf-16'))
        self.package()
        with zipfile.ZipFile(self.bundle()) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        with zipfile.ZipFile(self.bundle(), 'w') as archive:
            for name, data in members.items():
                archive.writestr(name + 'X' if name == 'app.bin' else name, data)
        self.bundle().write_bytes(self.bundle().read_bytes().replace(b'app.binX', b'app.bin\0'))
        with self.assertRaisesRegex(ValueError, 'truncated'):
            validate_bundle(self.bundle(), self.policy, TAG)

    def test_valid_hashes_do_not_authorize_overlap_partition_overflow_or_bad_padding(self):
        self.package()
        original = self.bundle().read_bytes()
        for offset, size, error in [(0, 0x9000, 'overlap'), (0x210000, 0x700001, 'main_app partition')]:
            self.bundle().write_bytes(original)

            def replace_app(manifest, members):
                data = b'\0' * size
                members['app.bin'] = data
                manifest['components'][1].update(target_offset=offset, size=size, sha256=fmt.sha256(data))

            self.rewrite(replace_app)
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                validate_bundle(self.bundle(), self.policy, TAG)
        self.bundle().write_bytes(original)

        def replace_padding(manifest, members):
            data = members['partition-table.bin'][:-1] + b'\0'
            members['partition-table.bin'] = data
            manifest['components'][0]['sha256'] = fmt.sha256(data)
            manifest['target_layout_sha256'] = fmt.sha256(data)

        self.rewrite(replace_padding)
        with self.assertRaisesRegex(ValueError, 'trailing data'):
            validate_bundle(self.bundle(), self.policy, TAG)

    def test_excluded_application_version_and_project_are_not_valid_iris_firmware(self):
        with self.assertRaisesRegex(ValueError, 'Empty or invalid application identity'):
            fmt.inspect_application(application(18, tag=''), 18)
        build = self.builds / self.board / self.policy['build_directory']
        config_path = build / 'sdkconfig'
        config_path.write_text(config_path.read_text() + 'CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR=y\n')
        with self.assertRaisesRegex(ValueError, 'identity is disabled'):
            record_build(build, self.policy, SOURCE, PIPELINE, 456, self.policy['idf_image'], 'b' * 40)

    def test_sidecars_and_completeness_are_revalidated(self):
        self.package()
        name = self.output / f'esp-vision-{self.board}-{TAG}.provenance.json'
        changed = json.loads(name.read_text())
        changed['build']['board'] = 'ESP32_S31_KORVO'  # Same chip is insufficient.
        name.write_bytes(fmt.json_bytes(changed))
        with self.assertRaisesRegex(ValueError, 'board'):
            validate_release(self.output, TAG, SOURCE, PIPELINE)

    def test_macro_and_retained_configuration_are_required(self):
        build = self.builds / self.board / self.policy['build_directory']
        cache = build / 'CMakeCache.txt'
        cache.write_text('ESP_VISION_ENABLE_IRISFW:BOOL=OFF\n')
        with self.assertRaisesRegex(ValueError, 'macro'):
            record_build(build, self.policy, SOURCE, PIPELINE, 456, self.policy['idf_image'], 'b' * 40)
        cache.write_text('ESP_VISION_ENABLE_IRISFW:BOOL=ON\n')
        (build / 'ota_data_initial.bin').write_bytes(b'\0' * 8192)
        with self.assertRaisesRegex(ValueError, 'bootstrap'):
            record_build(build, self.policy, SOURCE, PIPELINE, 456, self.policy['idf_image'], 'b' * 40)
        with self.assertRaisesRegex(ValueError, 'only for ESP32_S31_MOSAICO'):
            fmt.board_policy('ESP32_S31_KORVO')

    def test_other_boards_cannot_enable_the_compile_macro(self):
        result = subprocess.run(['cmake', '-DMICROPY_BOARD=ESP32_P4X_EYE', '-DESP_VISION_ENABLE_IRISFW=ON',
                                 '-P', str(fmt.ROOT / 'overlay/micropython/ports/esp32/CMakeLists.txt')],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('supported only on ESP32_S31_MOSAICO', result.stderr)

    def test_fat_uses_the_complete_tail_after_the_seven_mib_application(self):
        slot = fmt.application_partition(self.policy)
        fat = [p for p in self.policy['partitions'] if p[4] == 'ffat'][0]
        self.assertEqual(slot[3], 7 * 1024 * 1024)
        self.assertEqual(fat[2], slot[2] + slot[3])
        self.assertEqual(fat[2] + fat[3], self.policy['flash_size'])
        self.assertNotIn('ui_apps', [p[4] for p in self.policy['partitions']])
        root = self.root / 'policy-source'
        port = root / 'boards' / self.board / 'port'
        port.mkdir(parents=True)
        catalog = root / 'tools/ci/launchpad/boards.yml'
        catalog.parent.mkdir(parents=True)
        catalog.write_bytes((fmt.ROOT / 'tools/ci/launchpad/boards.yml').read_bytes())
        for name in ('sdkconfig.board', 'sdkconfig.irisfw', 'partitions-16MiB-irisfw.csv'):
            (port / name).write_bytes((fmt.ROOT / 'boards' / self.board / 'port' / name).read_bytes())
        table = port / 'partitions-16MiB-irisfw.csv'
        original = table.read_text()
        for invalid in (original.replace('0x6f0000', '0x6e0000'),
                        original.replace('0x6f0000', '0x700000'),
                        original + 'ui_apps,data,0x40,0xf00000,0x100000,\n'):
            table.write_text(invalid)
            with self.assertRaisesRegex(ValueError, 'application/storage layout mismatch'):
                fmt.board_policy(self.board, root)

    def test_retained_system_partitions_cannot_change_even_with_updated_hashes(self):
        self.package()
        def change_vibe(manifest, members):
            data = bytearray(members['partition-table.bin'])
            struct.pack_into('<I', data, 3 * 32 + 8, 0x1b0000)
            checksum = len(self.policy['partitions']) * 32
            data[checksum + 16:checksum + 32] = hashlib.md5(data[:checksum]).digest()
            members['partition-table.bin'] = bytes(data)
            manifest['target_layout_sha256'] = fmt.sha256(data)
            manifest['components'][0]['sha256'] = fmt.sha256(data)
        self.rewrite(change_vibe)
        with self.assertRaisesRegex(ValueError, 'layout differs'):
            validate_bundle(self.bundle(), self.policy, TAG)


class RegistryTests(unittest.TestCase):
    def test_resume_readback_conflicts_and_completion_order(self):
        class FakeRegistry:
            def __init__(self):
                self.files, self.writes, self.corrupt = {}, [], False

            def request(self, name, data=None):
                if data is None:
                    return self.files.get(name)
                self.writes.append(name)
                self.files[name] = b'corrupt' if self.corrupt else data

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in ('a.irisfw', 'SHA256SUMS', 'release.json'):
                (directory / name).write_bytes(name.encode())
            registry = FakeRegistry()
            registry.files['a.irisfw'] = b'a.irisfw'
            publish_files(directory, registry)
            self.assertEqual(registry.writes, ['SHA256SUMS', 'release.json'])
            publish_files(directory, registry)
            self.assertEqual(len(registry.writes), 2)
            registry.files['release.json'] = b'conflict'
            with self.assertRaisesRegex(ValueError, 'Immutable'):
                publish_files(directory, registry)
            self.assertEqual(len(registry.writes), 2)
            registry = FakeRegistry()
            registry.corrupt = True
            with self.assertRaisesRegex(ValueError, 'verification'):
                publish_files(directory, registry)
            self.assertNotIn('release.json', registry.files)

    def test_redirects_strip_cross_origin_credentials_and_reject_downgrades(self):
        handler = DownloadRedirects()
        req = Request('https://gitlab.example/package', headers={'JOB-TOKEN': 'test-secret'})
        redirect = handler.redirect_request(req, None, 302, 'Found', {}, 'https://objects.example/blob')
        self.assertNotIn('test-secret', str(redirect.headers))
        same = handler.redirect_request(req, None, 302, 'Found', {}, 'https://gitlab.example/blob')
        self.assertIn('test-secret', str(same.headers))
        with self.assertRaises(ValueError):
            handler.redirect_request(req, None, 302, 'Found', {}, 'http://gitlab.example/blob')


class PipelineTests(unittest.TestCase):
    def test_iris_reuses_release_pairs_and_cannot_bypass_qualification(self):
        ci = yaml.safe_load((fmt.ROOT / '.gitlab/ci/deploy.yml').read_text())
        self.assertNotEqual(ci['pack_irisfw']['needs'], ci['pack_launchpad']['needs'])
        expected = {'firmware_irisfw_mosaico'}
        self.assertEqual({item['job'] for item in ci['pack_irisfw']['needs']}, expected)
        for job in ('pack_irisfw', 'publish_irisfw'):
            self.assertEqual(ci[job]['extends'], '.rules:release')
        self.assertIn({'job': 'qualify_firmware_release', 'artifacts': False}, ci['publish_irisfw']['needs'])
        self.assertEqual(ci['publish_irisfw']['resource_group'], 'irisfw-publication')


if __name__ == '__main__':
    unittest.main()
