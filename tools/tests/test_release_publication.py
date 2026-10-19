# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise immutable publication, published-only history and both layouts."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ci/launchpad'))
import publication as P  # noqa: E402
import validate_release as V  # noqa: E402

COMMIT = 'a' * 40


def descriptor(data, **values):
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), **values)


def config_bytes(config):
    lines = ['[website]', 'release = ' + json.dumps(config['website']['release']),
             'releases = ' + json.dumps(config['website']['releases'])]
    if 'publication' in config:
        lines.append('[publication]')
        for key, value in config['publication'].items():
            if key != 'releases':
                lines.append(key + ' = ' + json.dumps(value))
        for record in config['publication']['releases']:
            lines.append('[[publication.releases]]')
            lines.extend(key + ' = ' + json.dumps(value) for key, value in record.items())
    return ('\n'.join(lines) + '\n').encode()


class ReleasePublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        app = b'qualified application'
        table = struct.pack('<HBBII16sI', 0x50AA, 0, 0, 65536, 4096, b'factory', 0)
        full = bytearray(b'\xff' * (65536 + len(app)))
        full[8192:8196], full[32768:32768 + len(table)], full[65536:] = b'boot', table, app
        self.entry = dict(board='BOARD', version='v1', firmwareVersion='v1', sourceCommit=COMMIT,
                          capabilities={'wifi': False},
                          image=descriptor(app, url=P.ROOT_URL + 'bin/app.bin', offset=65536,
                                           partitionSize=4096, chipFamily=0x3280, evMuxVersion=4,
                                           chipRevisionMin=301, chipRevisionMax=399,
                                           partitionTable=descriptor(table, offset=32768)),
                          install=descriptor(full, url=P.ROOT_URL + 'bin/full.bin', offset=0,
                                             flashSize=16 * 1024 * 1024, chipRevisionMin=301,
                                             chipRevisionMax=399, bootloader=descriptor(b'boot', offset=8192)))
        self.config = {'firmware_images_url': P.ROOT_URL + 'bin/', 'supported_apps': ['esp-vision'],
                       'esp-vision': {'image': {'BOARD': 'full.bin'}},
                       'website': {'schemaVersion': '1.0.0', 'release': 'v1', 'releases': ['v1'],
                                   'boards': [{'board': 'BOARD', 'flashable': True}],
                                   'models': [{'path': 'models/test.espdl', 'downloadUrl': 'mutable'}]}}
        self.previous = b'[website]\nrelease="old"\nreleases=["old", "unqualified-legacy"]\n'
        (self.root / 'full.bin').write_bytes(full)
        (self.root / 'app.bin').write_bytes(app)
        path = self.root / 'firmware/BOARD/v1.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(self.entry))

    def publish(self, **options):
        return P.write_publication(self.root, self.config, [self.entry], 'v1', COMMIT, 123,
                                   self.previous, allow_missing_mcp=True, **options)

    def test_legacy_compatibility_and_pinned_snapshot(self):
        manifest = self.publish()
        self.assertEqual(V.validate_release(self.root, 'v1', self.config, COMMIT, 123, True), 1)
        self.assertEqual(self.config['publication']['previousConfigSha256'], P.sha256(self.previous))
        self.assertEqual(self.config['publication']['releases'][0]['boards'], ['BOARD'])
        self.assertEqual(len(self.config['publication']['releases']), 1)
        self.assertIn('unqualified-legacy', manifest['website']['releases'])
        self.assertEqual(manifest['website']['models'][0]['downloadUrl'],
                         f'https://raw.githubusercontent.com/espressif/esp-vision/{COMMIT}/models/test.espdl')
        self.assertIsNone(manifest['mcp'])

    def test_single_bin_does_not_require_app_or_per_board_json(self):
        self.entry['image']['source'] = {key: self.entry['install'][key] for key in ('url', 'bytes', 'sha256')}
        self.entry['image']['source']['offset'] = self.entry['image']['offset']
        self.entry['image']['url'] = self.entry['install']['url']
        (self.root / 'app.bin').unlink()
        (self.root / 'firmware/BOARD/v1.json').unlink()
        self.publish(layout='single-bin')
        self.assertEqual(V.validate_release(self.root, 'v1', self.config, COMMIT, 123, True), 1)

    def test_container_hash_and_application_slice_both_checked(self):
        self.test_single_bin_does_not_require_app_or_per_board_json()
        manifest_path = self.root / 'releases/v1/manifest.json'
        for mutate in (lambda e: e['image']['source'].update(offset=65537),
                       lambda e: e['image'].update(sha256='0' * 64),
                       lambda e: e['image']['source'].update(sha256='0' * 64)):
            original = json.loads(manifest_path.read_text())
            manifest = copy.deepcopy(original)
            mutate(manifest['releases'][0])
            data = json.dumps(manifest).encode()
            manifest_path.write_bytes(data)
            self.config['publication']['manifestSha256'] = P.sha256(data)
            self.config['publication']['releases'][0]['manifestSha256'] = P.sha256(data)
            with self.assertRaises(ValueError):
                V.validate_release(self.root, 'v1', self.config)
            manifest_path.write_text(json.dumps(original))

    def test_history_preserves_only_verified_published_records(self):
        manifest = self.publish()
        raw = (self.root / 'releases/v1/manifest.json').read_bytes()
        self.previous = config_bytes(self.config)
        self.entry['version'] = self.entry['firmwareVersion'] = 'v2'
        self.config['website']['release'] = 'v2'
        old_records = copy.deepcopy(self.config['publication']['releases'])
        P.write_publication(self.root, self.config, [self.entry], 'v2', COMMIT, 124, self.previous,
                            fetch=lambda url: raw, allow_missing_mcp=True)
        self.assertEqual(self.config['publication']['releases'][1:], old_records)
        self.assertEqual(self.config['publication']['releases'][0]['boards'], ['BOARD'])
        self.assertEqual(manifest['version'], 'v1')

    def test_history_fetch_hash_or_membership_failure_aborts(self):
        self.publish()
        raw = (self.root / 'releases/v1/manifest.json').read_bytes()
        previous = config_bytes(self.config)
        with self.assertRaises(OSError):
            P.previous_publication(previous, fetch=lambda _: (_ for _ in ()).throw(OSError('offline')))
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            P.previous_publication(previous, fetch=lambda _: raw + b'corrupt')
        config = copy.deepcopy(self.config)
        config['publication']['releases'][0]['boards'] = ['OTHER']
        with self.assertRaisesRegex(ValueError, 'membership'):
            P.previous_publication(config_bytes(config), fetch=lambda _: raw)

    def test_published_tag_is_immutable(self):
        self.publish()
        raw = (self.root / 'releases/v1/manifest.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'immutable'):
            P.write_publication(self.root, self.config, [self.entry], 'v1', COMMIT, 124,
                                config_bytes(self.config), fetch=lambda _: raw, allow_missing_mcp=True)

    def test_website_cannot_offer_unpublished_board(self):
        self.config['website']['boards'].append({'board': 'ABSENT', 'flashable': True})
        with self.assertRaisesRegex(ValueError, 'Flashable website board'):
            self.publish()

    def test_mcp_requires_exact_current_pack_and_explicit_absence(self):
        with self.assertRaisesRegex(ValueError, 'MCP pack missing'):
            P.write_publication(self.root, self.config, [self.entry], 'v1', COMMIT, 123, self.previous)
        directory = self.root / 'mcp'
        directory.mkdir()
        data = json.dumps({'meta': {'version': 'v1', 'sourceCommit': COMMIT}}).encode()
        (directory / 'mcp-pack-v1.json').write_bytes(data)
        (directory / 'latest.json').write_bytes(data)
        manifest = self.publish()
        self.assertEqual(manifest['mcp']['sha256'], P.sha256(data))
        self.assertEqual(V.validate_release(self.root, 'v1', self.config), 1)
        (directory / 'mcp-pack-v1.json').write_text('{"meta":{"version":"old","sourceCommit":"bad"}}')
        with self.assertRaisesRegex(ValueError, 'MCP build identity'):
            self.publish()

    def test_unknown_capability_or_wrong_pipeline_cannot_qualify(self):
        self.entry['capabilities']['wifi'] = None
        with self.assertRaises(ValueError):
            self.publish()
        self.entry['capabilities']['wifi'] = False
        self.publish()
        with self.assertRaisesRegex(ValueError, 'pipeline'):
            V.validate_release(self.root, 'v1', self.config, pipeline_id=999)


if __name__ == '__main__':
    unittest.main()
