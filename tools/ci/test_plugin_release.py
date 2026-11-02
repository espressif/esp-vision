# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import plugin_release


class PluginReleaseTests(unittest.TestCase):
    def test_verification_rejects_wrong_commit_and_modified_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for platform in ['workbuddy-expert', 'qoderwork', 'cursor']:
                path = root / f'esp-vision-{platform}-0.1.0.zip'
                with zipfile.ZipFile(path, 'w') as bundle:
                    bundle.writestr('plugin/BUILD-PROVENANCE.json', json.dumps({'sourceCommit': 'a' * 40}))
                Path(str(path) + '.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name + '\n')
            self.assertEqual(len(plugin_release.verify(root, 'a' * 40)), 6)
            with self.assertRaisesRegex(ValueError, 'Source mismatch'):
                plugin_release.verify(root, 'b' * 40)
            path.write_bytes(path.read_bytes() + b'changed')
            with self.assertRaisesRegex(ValueError, 'Checksum mismatch'):
                plugin_release.verify(root, 'a' * 40)

    def test_existing_identical_assets_are_reused_and_conflicts_stop_upload(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'package.zip'
            path.write_bytes(b'original')
            release = {'id': 1, 'upload_url': 'https://uploads.example/assets{?name}', 'draft': False}
            assets = [{'name': path.name, 'url': 'https://api.example/asset'}]
            with patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo', 'GH_TOKEN': 'test'}):
                with patch('urllib.request.urlopen', side_effect=[io.BytesIO(json.dumps(release).encode()), io.BytesIO(json.dumps(assets).encode()), io.BytesIO(b'original')]) as request:
                    plugin_release.publish([path], 'v1.0.0', 'a' * 40)
                    self.assertEqual(request.call_count, 3)
                with patch('urllib.request.urlopen', side_effect=[io.BytesIO(json.dumps(release).encode()), io.BytesIO(json.dumps(assets).encode()), io.BytesIO(b'other')]) as request:
                    with self.assertRaisesRegex(ValueError, 'Existing release asset differs'):
                        plugin_release.publish([path], 'v1.0.0', 'a' * 40)
                    self.assertEqual(request.call_count, 3)

    def test_missing_release_creates_draft_and_uploads_original_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'package.zip'
            path.write_bytes(b'original')
            release = {'id': 1, 'upload_url': 'https://uploads.example/assets{?name}'}
            missing = plugin_release.urllib.error.HTTPError('https://api.example', 404, 'missing', {}, None)
            with patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo', 'GH_TOKEN': 'test'}):
                with patch('urllib.request.urlopen', side_effect=[missing, io.BytesIO(json.dumps(release).encode()), io.BytesIO(b'[]'), io.BytesIO(b'{}')]) as request:
                    plugin_release.publish([path], 'v1.0.0', 'a' * 40)
                    created = json.loads(request.call_args_list[1].args[0].data)
                    self.assertTrue(created['draft'])
                    self.assertEqual(created['tag_name'], 'v1.0.0')
                    self.assertEqual(request.call_args_list[3].args[0].data, b'original')


if __name__ == '__main__':
    unittest.main()
