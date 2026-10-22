# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Producer contract and publication fault regression tests (no registry writes)."""
import io
import json
import tarfile
import unittest
from urllib.request import Request

from release import StripRedirectCredentials, descriptor, publish, release_files, sha256


class MemoryRegistry:
    def __init__(self):
        self.data = {}
        self.writes = []

    def file(self, version, name, data=None):
        if data is not None:
            self.writes.append(name)
            self.data[(version, name)] = data
        return self.data.get((version, name))


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.version = '2026.10.21'
        self.commit = 'a' * 40
        self.pack = json.dumps({'meta': {'version': self.version, 'sourceCommit': self.commit}}).encode()
        self.digest = 'sha256:' + 'b' * 64
        self.manifest = descriptor(self.pack, self.version, self.commit, 'registry.example/project/knowledge-mcp', self.digest)
        self.files = release_files(self.pack, self.manifest)

    def test_descriptor_binds_exact_bytes_source_and_immutable_image(self):
        self.assertEqual(self.manifest['knowledge']['sha256'], sha256(self.pack))
        self.assertEqual(self.manifest['knowledge']['bytes'], len(self.pack))
        self.assertTrue(self.manifest['image']['reference'].endswith('@' + self.digest))
        self.assertEqual(self.files['mcp-release.sha256'].decode().strip(), sha256(self.files['mcp-release.json']))

    def test_rejects_wrong_source_version_or_mutable_image(self):
        for version, commit, digest in [('other', self.commit, self.digest),
                                        (self.version, 'c' * 40, self.digest),
                                        (self.version, self.commit, 'latest')]:
            with self.subTest(version=version, commit=commit, digest=digest), self.assertRaises(ValueError):
                descriptor(self.pack, version, commit, 'registry.example/project/mcp', digest)

    def test_idempotent_publish_and_partial_recovery(self):
        registry = MemoryRegistry()
        registry.data[(self.version, 'mcp-pack.json')] = self.pack
        publish(registry, self.version, self.files)
        self.assertEqual(registry.writes[-1], 'mcp-release.sha256')
        self.assertNotIn('mcp-pack.json', registry.writes)
        before = list(registry.writes)
        publish(registry, self.version, self.files)
        self.assertEqual(registry.writes, before)

    def test_conflicting_file_prevents_all_writes(self):
        registry = MemoryRegistry()
        registry.data[(self.version, 'mcp-release.json')] = b'changed'
        with self.assertRaises(ValueError):
            publish(registry, self.version, self.files)
        self.assertEqual(registry.writes, [])

    def npm_tarball(self, *, pack=None, provenance=None, package=None, link=False):
        pack = self.pack if pack is None else pack
        metadata = {'name': '@esp-vision/knowledge-mcp', 'version': '0.1.0'}
        metadata.update(package or {})
        identity = {'schemaVersion': 1, 'packageName': metadata['name'],
                    'packageVersion': metadata['version'], 'packVersion': self.version,
                    'sourceCommit': self.commit, 'packSha256': sha256(pack)}
        identity.update(provenance or {})
        files = {'package/package.json': json.dumps(metadata).encode(),
                 'package/data/mcp-pack.json': pack,
                 'package/data/provenance.json': json.dumps(identity).encode()}
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as archive:
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            if link:
                info = tarfile.TarInfo('package/unsafe-link')
                info.type = tarfile.SYMTYPE
                info.linkname = '/outside'
                archive.addfile(info)
        return output.getvalue()

    def test_schema2_binds_the_exact_npm_artifact_and_snapshot(self):
        archive = self.npm_tarball()
        manifest = descriptor(self.pack, self.version, self.commit,
                              'registry.example/project/knowledge-mcp', self.digest, archive)
        self.assertEqual(manifest['schemaVersion'], 2)
        self.assertEqual(manifest['npm'], {'name': '@esp-vision/knowledge-mcp',
                         'version': '0.1.0', 'file': 'knowledge-mcp.tgz',
                         'sha256': sha256(archive), 'bytes': len(archive)})
        files = release_files(self.pack, manifest, archive)
        self.assertEqual(files['knowledge-mcp.tgz'], archive)
        registry = MemoryRegistry()
        publish(registry, self.version, files)
        self.assertEqual(registry.writes[-1], 'mcp-release.sha256')
        self.assertEqual(registry.writes[0], 'knowledge-mcp.tgz')

    def test_schema2_rejects_wrong_bytes_provenance_and_unsafe_archives(self):
        for archive in [self.npm_tarball(pack=b'changed'),
                        self.npm_tarball(provenance={'sourceCommit': 'c' * 40}),
                        self.npm_tarball(package={'private': True}),
                        self.npm_tarball(package={'name': '@other/knowledge'}),
                        self.npm_tarball(link=True)]:
            with self.subTest(archive=sha256(archive)), self.assertRaises(ValueError):
                descriptor(self.pack, self.version, self.commit,
                           'registry.example/project/knowledge-mcp', self.digest, archive)

    def test_schema2_publication_refuses_a_different_archive(self):
        archive = self.npm_tarball()
        manifest = descriptor(self.pack, self.version, self.commit,
                              'registry.example/project/knowledge-mcp', self.digest, archive)
        with self.assertRaises(ValueError):
            release_files(self.pack, manifest, self.npm_tarball(provenance={'extra': 'changed'}))

    def test_redirect_drops_credentials_only_across_origins(self):
        handler = StripRedirectCredentials()
        req = Request('https://gitlab.example/api/file', headers={'JOB-TOKEN': 'secret'})
        other = handler.redirect_request(req, None, 302, '', {}, 'https://storage.example/file')
        same = handler.redirect_request(req, None, 302, '', {}, 'https://gitlab.example/new')
        self.assertNotIn('Job-token', other.headers)
        self.assertEqual(same.headers['Job-token'], 'secret')


if __name__ == '__main__':
    unittest.main()
