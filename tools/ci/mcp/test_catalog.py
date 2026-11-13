# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import copy
import io
import json
import tarfile
import unittest
from catalog import archive_catalog, encode, identity, preserve_history, snapshot_from_pack, validate_catalog
from release import descriptor, release_files, sha256


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.pack = encode({'meta': {'version': '2026.10.21', 'sourceCommit': 'a' * 40},
                            'modules': [], 'capabilities': [], 'docs': [], 'examples': []})
        self.snapshot = snapshot_from_pack(self.pack)
        self.catalog = {'schemaVersion': 1, 'latestFirmwareVersion': '2026.10.21', 'activeSnapshots': {'2026.10.21': '2026.10.21'}, 'snapshots': [self.snapshot]}
        self.data = encode(self.catalog)

    def archive(self, catalog=True, provenance=None):
        files = {'package.json': encode({'name': '@esp-vision/knowledge-mcp', 'version': '1.0.0'}),
                 'data/provenance.json': encode(provenance or {'schemaVersion': 2, 'packageName': '@esp-vision/knowledge-mcp',
                    'packageVersion': '1.0.0', 'sourceCommit': 'b' * 40, 'catalogSha256': sha256(self.data)})}
        files['data/mcp-catalog.json' if catalog else 'data/mcp-pack.json'] = self.data if catalog else self.pack
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as tar:
            for name, data in files.items():
                member = tarfile.TarInfo('package/' + name)
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
        return output.getvalue()

    def test_runtime_and_firmware_commits_are_independent_and_descriptor_is_complete(self):
        archive = self.archive()
        result = descriptor(self.data, '1.0.0', 'b' * 40, 'registry.example/knowledge', 'sha256:' + 'c' * 64, archive)
        self.assertEqual(result['schemaVersion'], 3)
        self.assertEqual(result['latestFirmwareVersion'], '2026.10.21')
        self.assertEqual(result['version'], result['npm']['version'])
        self.assertEqual(result['knowledge']['file'], 'mcp-catalog.json')
        self.assertEqual(result['snapshots'], [{**identity(self.snapshot), 'active': True}])
        files = release_files(self.data, result, archive)
        self.assertEqual(files['mcp-catalog.json'], self.data)
        self.assertEqual(list(files)[-1], 'mcp-release.sha256')
        self.assertNotIn('mcp-pack.json', files)
        with self.assertRaises(ValueError):
            descriptor(self.data, '2026.10.21', 'b' * 40, 'registry.example/knowledge', 'sha256:' + 'c' * 64, archive)

    def test_historical_npm_migration_and_append_only_retention(self):
        previous = archive_catalog(self.archive(catalog=False))
        preserve_history(previous, validate_catalog(self.data))
        for mutation in ('remove', 'rewrite'):
            changed = copy.deepcopy(self.catalog)
            if mutation == 'remove':
                changed['snapshots'] = []
            else:
                changed['snapshots'][0]['packJson'] += ' '
            with self.assertRaises(ValueError):
                preserve_history(previous, changed)

    def test_revisions_keep_original_firmware_identity_and_old_revision_readable(self):
        updated = json.loads(self.pack)
        updated['meta']['sourceCommit'] = 'd' * 40
        revised = snapshot_from_pack(encode(updated), 'a' * 40, '2026.10.21-r2')
        current = copy.deepcopy(self.catalog)
        current['snapshots'].append(revised)
        current['activeSnapshots']['2026.10.21'] = revised['snapshotVersion']
        preserve_history(self.catalog, validate_catalog(encode(current)))
        self.assertNotEqual(revised['firmwareSourceCommit'], revised['contentSourceCommit'])
        current['snapshots'][1]['firmwareSourceCommit'] = 'f' * 40
        with self.assertRaises(ValueError):
            validate_catalog(encode(current))

    def test_latest_alias_is_not_an_official_firmware_even_with_a_matching_snapshot(self):
        pack = json.loads(self.pack)
        pack['meta']['version'] = 'latest'
        snapshot = snapshot_from_pack(encode(pack))
        with self.assertRaises(ValueError):
            validate_catalog(encode({'schemaVersion': 1, 'latestFirmwareVersion': 'latest',
                                     'activeSnapshots': {'latest': 'latest'}, 'snapshots': [snapshot]}))

    def test_corrupt_catalogs_fail_closed(self):
        for mutate in [lambda c: c.pop('latestFirmwareVersion'),
                       lambda c: c.update({'latestFirmwareVersion': 'missing'}),
                       lambda c: c.update({'latestFirmwareVersion': 'latest'}),
                       lambda c: c['snapshots'].append(c['snapshots'][0]),
                       lambda c: c['activeSnapshots'].clear(),
                       lambda c: c['activeSnapshots'].update({'2026.10.21': 'latest'}),
                       lambda c: c['snapshots'][0].update({'packSha256': '0' * 64})]:
            changed = copy.deepcopy(self.catalog)
            mutate(changed)
            with self.assertRaises(ValueError):
                validate_catalog(encode(changed))


if __name__ == '__main__':
    unittest.main()
