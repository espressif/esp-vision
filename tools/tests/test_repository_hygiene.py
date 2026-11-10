# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Exercise the repository boundary through real Git indexes and ignore rules."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECK = ROOT / 'tools/ci/check_repository_hygiene.py'


class RepositoryHygieneTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.env = os.environ.copy()
        # pre-commit may export the parent index; each fixture owns its own repo.
        for name in ('GIT_INDEX_FILE', 'GIT_DIR', 'GIT_WORK_TREE', 'GIT_COMMON_DIR'):
            self.env.pop(name, None)
        self.git('init', '-q')
        shutil.copyfile(ROOT / '.gitignore', self.repo / '.gitignore')

    def git(self, *args, check=True):
        return subprocess.run(
            ['git', *args], cwd=self.repo, env=self.env, check=check,
            capture_output=True, text=True,
        )

    def stage(self, path):
        file = self.repo / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text('fixture\n', encoding='utf-8')
        self.git('add', '-f', '--', path)

    def check(self):
        return subprocess.run(
            [sys.executable, str(CHECK)], cwd=self.repo, env=self.env,
            capture_output=True, text=True,
        )

    def test_force_added_ignored_artifacts_are_rejected(self):
        paths = [
            'specs/native-uvc.md', '.specs/design.md', 'plans/next.md',
            'scratch/probe.py', 'investigations/results.json', 'handoffs/task.md',
            '.codex/config.toml', '.claude/settings.json', '.cursor/rules/local.md',
            '.agents/memory.md', '.vscode/settings.json', '.idea/workspace.xml',
            '.esp-vision/results.json', '.worktrees/task/file.md',
            '.env', 'host/device-core/.env.local', '.npmrc', '.pypirc', '.netrc',
            '.mcp.json', 'host/device-core/specs/draft.md',
            'host/esp-vision-plugin/.agents/private.json',
            'host/esp-vision-plugin/.agents/plugins/local.json',
        ]
        for path in paths:
            with self.subTest(path=path):
                self.assertEqual(self.git('check-ignore', '-q', '--', path, check=False).returncode, 0)
                self.stage(path)
                result = self.check()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(repr(path), result.stderr)
                self.git('rm', '--cached', '--', path)

    def test_public_sources_and_sanitized_templates_are_allowed(self):
        paths = [
            'AGENTS.md', 'docs/en/architecture/index.rst', 'tools/tests/fixtures/protocol.json',
            'host/esp-vision-plugin/.agents/plugins/marketplace.json',
            'host/esp-vision-plugin/.mcp.json',
            'host/esp-vision-plugin/.claude-plugin/plugin.json',
            'host/esp-vision-plugin/skills/esp-vision-development/SKILL.md',
            '.env.example', 'host/device-core/.env.template',
        ]
        for path in paths:
            with self.subTest(path=path):
                self.assertEqual(self.git('check-ignore', '-q', '--', path, check=False).returncode, 1)
                self.stage(path)
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_untracked_local_artifacts_do_not_block(self):
        (self.repo / 'specs').mkdir()
        (self.repo / 'specs/draft.md').write_text('local only\n', encoding='utf-8')
        self.assertEqual(self.check().returncode, 0)

    def test_staged_removal_resolves_violation(self):
        self.stage('specs/draft.md')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture')
        self.assertEqual(self.check().returncode, 1)
        self.git('rm', '--', 'specs/draft.md')
        self.assertEqual(self.check().returncode, 0)

    def test_unstaged_deletion_still_fails(self):
        self.stage('specs/draft.md')
        (self.repo / 'specs/draft.md').unlink()
        self.assertEqual(self.check().returncode, 1)

    def test_whitespace_in_names_does_not_bypass_check(self):
        for path in ('specs/a draft.md', 'specs/line\nbreak.md'):
            self.stage(path)
            result = self.check()
            self.assertEqual(result.returncode, 1)
            self.assertIn(repr(path), result.stderr)

    def test_missing_git_repository_fails_closed(self):
        self.env['GIT_CEILING_DIRECTORIES'] = str(self.repo.parent)
        shutil.rmtree(self.repo / '.git')
        self.assertEqual(self.check().returncode, 2)


if __name__ == '__main__':
    unittest.main()
