# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Expert publication fault tests use only an in-memory registry/release API."""
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from release import sha256
from workbuddy_release import ExpertRegistry, checked_files, publish_release, require_publish_ci


def write_expert_artifact(directory, version, tag, commit, display_name=None, metadata=None):
    filename = f'esp-vision-workbuddy-expert-{version}.zip'
    build = {'expertVersion': version, 'sourceVersion': '0.1.0',
             'sourceCommit': commit, 'releaseTag': tag, 'pipelineId': None}
    manifest = {'version': version, 'displayName': display_name if display_name is not None
                else {'en': 'ESP-Vision AI', 'zh': 'ESP-Vision AI视觉'}}
    template = json.loads((Path(__file__).resolve().parents[3]
                           / 'host/esp-vision-plugin/workbuddy/expert/plugin.json').read_text())
    manifest.update({key: template[key] for key in ('displayDescription', 'keywords', 'tags')})
    manifest.update(metadata or {})
    entries = {'BUILD-PROVENANCE.json': json.dumps(build).encode(),
               '.codebuddy-plugin/plugin.json': json.dumps(manifest).encode()}
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as bundle:
        for path, data in entries.items():
            bundle.writestr('esp-vision-workbuddy/' + path, data)
    archive = output.getvalue()
    receipt = {'archive': filename, 'expertVersion': version, 'sourceVersion': '0.1.0',
               'sha256': sha256(archive), 'build': build,
               'inventory': [{'path': path, 'bytes': len(data), 'sha256': sha256(data)}
                             for path, data in entries.items()]}
    (directory / filename).write_bytes(archive)
    (directory / (filename + '.sha256')).write_text(f'{sha256(archive)}  {filename}\n')
    receipt_path = directory / (filename + '.receipt.json')
    receipt_path.write_text(json.dumps(receipt))
    return receipt_path, receipt


class MemoryRegistry:
    base = 'https://gitlab.example/api/v4/projects/42'
    package_path = staticmethod(ExpertRegistry.package_path)

    def __init__(self, commit):
        self.commit = commit
        self.tag_exists = True
        self.fail_readback = False
        self.release = {'commit': {'id': commit}, 'description': 'Keep release notes'}
        self.links = [{'name': 'Firmware', 'url': 'https://firmware.example/bin'}]
        self.data = {}
        self.writes = []
        self.posts = []

    def file(self, version, name, data=None):
        if data is not None:
            self.writes.append(name)
            self.data[(version, name)] = data
        result = self.data.get((version, name))
        return b'failed readback' if self.fail_readback and result is not None else result

    def api(self, path, data=None):
        if path.startswith('/repository/tags/'):
            return {'commit': {'id': self.commit}} if self.tag_exists else None
        if data is not None:
            self.posts.append((path, data))
            if path == '/releases':
                self.release = {'commit': {'id': self.commit}, **data}
                return self.release
            self.links.append(data)
            return data
        return self.links if path.endswith('/assets/links') else self.release


class ExpertReleaseTests(unittest.TestCase):
    def setUp(self):
        self.commit = 'a' * 40
        self.tag = '2026.10.27'
        self.version = '2026.10.27'
        self.files = {'expert.zip': b'zip', 'expert.zip.sha256': b'hash', 'expert.zip.receipt.json': b'receipt'}

    def test_immutable_idempotent_cohort_and_release_links_preserve_existing_notes(self):
        registry = MemoryRegistry(self.commit)
        publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.writes, list(self.files))
        self.assertEqual(registry.release['description'], 'Keep release notes')
        self.assertEqual(registry.links[0]['name'], 'Firmware')
        self.assertEqual(len(registry.links), 4)
        before = list(registry.posts)
        publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.posts, before)
        self.assertEqual(registry.writes, list(self.files))

    def test_conflicting_bytes_or_tag_source_never_overwrite_registry_files(self):
        registry = MemoryRegistry(self.commit)
        registry.data[(self.tag, 'expert.zip')] = b'other'
        with self.assertRaises(ValueError):
            publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.writes, [])
        registry = MemoryRegistry('b' * 40)
        with self.assertRaisesRegex(ValueError, 'source commit'):
            publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.writes, [])

    def test_publication_requires_exact_protected_ci_identity(self):
        env = {'CI_COMMIT_REF_PROTECTED': 'true', 'CI_COMMIT_TAG': self.tag, 'CI_COMMIT_SHA': self.commit}
        require_publish_ci(env, self.tag, self.commit)
        for key in env:
            with self.subTest(key=key), self.assertRaises(ValueError):
                require_publish_ci({**env, key: ''}, self.tag, self.commit)

    def test_missing_release_is_created_without_a_new_tag_or_ref(self):
        registry = MemoryRegistry(self.commit)
        registry.release = None
        publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.posts[0][0], '/releases')
        self.assertEqual(registry.posts[0][1]['tag_name'], self.tag)
        self.assertNotIn('ref', registry.posts[0][1])

    def test_package_conflict_or_failed_readback_never_creates_a_release(self):
        for failed_readback in (False, True):
            with self.subTest(failed_readback=failed_readback):
                registry = MemoryRegistry(self.commit)
                registry.release = None
                registry.fail_readback = failed_readback
                if not failed_readback:
                    registry.data[(self.version, 'expert.zip')] = b'conflict'
                with self.assertRaises((ValueError, RuntimeError)):
                    publish_release(registry, self.tag, self.commit, self.version, self.files)
                self.assertIsNone(registry.release)
                self.assertEqual(registry.posts, [])

    def test_missing_git_tag_never_creates_a_release_or_package(self):
        registry = MemoryRegistry(self.commit)
        registry.release = None
        registry.tag_exists = False
        with self.assertRaisesRegex(ValueError, 'Existing Git tag'):
            publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.posts, [])
        self.assertEqual(registry.writes, [])

    def test_conflicting_existing_asset_is_rejected_before_any_package_or_release_write(self):
        registry = MemoryRegistry(self.commit)
        registry.links.append({'name': 'expert.zip', 'url': 'https://other.example/expert.zip'})
        with self.assertRaisesRegex(ValueError, 'different Expert asset'):
            publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.writes, [])
        self.assertEqual(registry.posts, [])

    def test_normalized_tag_aliases_cannot_publish_different_bytes_for_one_expert_version(self):
        registry = MemoryRegistry(self.commit)
        publish_release(registry, 'v1.2', self.commit, '1.2.0', self.files)
        self.assertIn(('1.2.0', 'expert.zip'), registry.data)
        self.assertTrue(all('/1.2.0/' in item['url'] for item in registry.links[1:]))
        before = list(registry.posts)
        registry.release = None  # The alias tag has not acquired a Release.
        with self.assertRaises(ValueError):
            publish_release(registry, 'v1.2.0', self.commit, '1.2.0', {**self.files, 'expert.zip': b'changed'})
        self.assertEqual(registry.posts, before)
        self.assertIsNone(registry.release)
        self.assertEqual(registry.data[('1.2.0', 'expert.zip')], b'zip')

    def test_partial_upload_recovers_but_conflicting_asset_link_is_never_replaced(self):
        registry = MemoryRegistry(self.commit)
        registry.data[(self.tag, 'expert.zip')] = b'zip'
        publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertNotIn('expert.zip', registry.writes)
        registry.links[-1]['url'] = 'https://other.example/replaced'
        with self.assertRaisesRegex(ValueError, 'different Expert asset'):
            publish_release(registry, self.tag, self.commit, self.version, self.files)
        self.assertEqual(registry.links[-1]['url'], 'https://other.example/replaced')

    def test_artifact_validation_binds_zip_manifest_inventory_and_full_source_identity(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            receipt_path, receipt = write_expert_artifact(directory, self.version, self.tag, self.commit)
            version, files = checked_files(directory, self.tag, self.commit)
            self.assertEqual(version, self.version)
            self.assertEqual(len(files), 3)
            with self.assertRaises(ValueError):
                checked_files(directory, self.tag, 'b' * 40)
            receipt['build']['releaseTag'] = None
            receipt_path.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, 'fixed tag/source'):
                checked_files(directory, self.tag, self.commit)

    def test_release_artifact_version_limit_accepts_20_and_rejects_21_characters(self):
        for length in (20, 21):
            version = '1.2.3-' + 'a' * (length - 6)
            tag = 'v' + version
            with self.subTest(length=length), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                write_expert_artifact(directory, version, tag, self.commit)
                if length == 20:
                    self.assertEqual(checked_files(directory, tag, self.commit)[0], version)
                else:
                    with self.assertRaisesRegex(ValueError, 'at most 20 characters'):
                        checked_files(directory, tag, self.commit)

    def test_release_names_accept_15_and_reject_16_and_reported_18_characters(self):
        for locale in ('zh', 'en'):
            for display_name in ('x' * 15, 'x' * 16, 'ESP-Vision AI 视觉开发'):
                with self.subTest(locale=locale, display_name=display_name), tempfile.TemporaryDirectory() as name:
                    directory = Path(name)
                    names = {'en': 'ESP-Vision AI', 'zh': 'ESP-Vision AI视觉', locale: display_name}
                    write_expert_artifact(directory, self.version, self.tag, self.commit, names)
                    if len(display_name) <= 15:
                        self.assertEqual(checked_files(directory, self.tag, self.commit)[0], self.version)
                    else:
                        with self.assertRaisesRegex(ValueError, rf'displayName\.{locale}.*at most 15 characters'):
                            checked_files(directory, self.tag, self.commit)

    def test_release_rejects_scope_metadata_regressions_in_actual_zip(self):
        for length in (40, 50):
            with self.subTest(length=length), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                write_expert_artifact(directory, self.version, self.tag, self.commit,
                                      metadata={'displayDescription': {'zh': '基于 ESP32' + 'x' * (length - 8)}})
                self.assertEqual(checked_files(directory, self.tag, self.commit)[0], self.version)
        cases = [
            ({'displayDescription': {'zh': '视觉应用开发专家' * 6}}, 'Chinese description'),
            ({'displayDescription': {'zh': '基于 ESP32' + 'x' * 31}}, 'Chinese description'),
            ({'displayDescription': {'zh': '基于 ESP32' + 'x' * 43}}, 'Chinese description'),
            ({'keywords': ['MicroPython']}, 'keywords'),
            ({'tags': [{'zh': 'ESP32', 'en': 'Vision'}]}, 'bilingual tags'),
            ({'tags': [{'zh': '视觉', 'en': 'ESP32'}]}, 'bilingual tags'),
        ]
        for metadata, error in cases:
            with self.subTest(metadata=metadata), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                write_expert_artifact(directory, self.version, self.tag, self.commit, metadata=metadata)
                with self.assertRaisesRegex(ValueError, error):
                    checked_files(directory, self.tag, self.commit)
