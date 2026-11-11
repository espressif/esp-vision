#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Check that the sync adapter preserves GitHub PR metadata without Jira."""

import importlib.util
import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock, mock_open, patch


ACTION = Path(__file__).resolve().parents[2] / '.github/actions/sync-pr-to-gitlab/sync_pr_to_gitlab/github_pr_to_internal_pr.py'


class SyncMetadataTest(unittest.TestCase):
    def test_creates_mr_for_pr_without_jira(self):
        event = {
            'repository': {'full_name': 'espressif/esp-vision'},
            'label': {'name': 'PR-Sync-Merge'},
            'sender': {'login': 'maintainer'},
            'pull_request': {
                'labels': [{'name': 'PR-Sync-Merge'}],
                'user': {'login': 'contributor'},
                'comments_url': 'https://example.test/comments',
                'number': 42,
                'url': 'https://example.test/pr/42',
                'html_url': 'https://github.com/espressif/esp-vision/pull/42',
                'base': {'ref': 'master'},
                'title': 'Fix camera startup',
                'body': None,
            },
        }
        env = {
            'GITHUB_TOKEN': 'test', 'GITLAB_URL': 'https://example.test', 'GITLAB_TOKEN': 'test',
            'GITLAB_NAMESPACE': 'ae_group', 'GITHUB_REPOSITORY': 'espressif/esp-vision',
            'GITHUB_EVENT_PATH': 'event.json',
        }
        dependencies = {
            'gitlab': MagicMock(),
            'gitlab.exceptions': MagicMock(GitlabGetError=Exception),
            'git': MagicMock(),
            'requests': MagicMock(),
        }
        with patch.dict(os.environ, env, clear=True), patch.dict(sys.modules, dependencies):
            spec = importlib.util.spec_from_file_location('sync_adapter', ACTION)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            gl = MagicMock()
            project = gl.projects.get.return_value
            mr = project.mergerequests.create.return_value
            with patch('builtins.open', mock_open(read_data=json.dumps(event))), \
                    patch.object(module, 'pr_check_approver', return_value='1234567'), \
                    patch.object(module, 'pr_check_forbidden_files'), \
                    patch.object(module, 'setup_project', return_value=gl), \
                    patch.object(module, 'sync_pr'):
                module.main()
            project.mergerequests.create.assert_called_once_with({
                'source_branch': 'contrib/github_pr_42', 'target_branch': 'master',
                'title': 'Fix camera startup (GitHub PR)', 'remove_source_branch': True,
            })
            self.assertIn('https://github.com/espressif/esp-vision/pull/42', mr.description)
            self.assertNotIn('Jira', mr.description)
            self.assertNotIn('Closes ', mr.description)
            mr.save.assert_called_once()


if __name__ == '__main__':
    unittest.main()
