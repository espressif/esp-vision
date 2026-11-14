# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Verify root IDF action routing without building firmware or opening a port."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('esp_vision_idf_ext_test', ROOT / 'idf_ext.py')
extension = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'idf_py_actions.errors': SimpleNamespace(FatalError=RuntimeError)}):
    spec.loader.exec_module(extension)


class IdfAppFlashTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='idf-app-flash-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'selected-build'
        self.build.mkdir()
        self.board = 'ESP32_S31_MOSAICO'
        for board in (self.board, 'ESP32_P4X_EYE'):
            port = self.root / 'boards' / board / 'port'
            port.mkdir(parents=True)
            (port / 'mpconfigboard.cmake').touch()
            (port.parent / 'manifest.py').touch()
        overlay = self.root / 'overlay/component_yml/release6.1'
        overlay.mkdir(parents=True)
        (overlay / 'idf_component.yml').touch()
        self.args = SimpleNamespace(board=self.board, project_dir=str(self.root),
                                    build_dir=str(self.build), define_cache_entry=[], dry_run=True)
        self.invoked = []
        self.ctx = SimpleNamespace(command=SimpleNamespace(get_command=lambda ctx, name: name),
                                   invoke=self.invoke)
        self.env = patch.dict(os.environ, {'ESP_IDF_VERSION': '6.1'})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.callback = extension.action_extensions({}, str(self.root))['global_action_callbacks'][0]

    def invoke(self, name):
        self.invoked.append(name)
        # IDF builds dependencies only after running global action callbacks.
        return SimpleNamespace(name=name, action_args={'default-option': True},
                               dependencies=['app'], order_dependencies=['erase-flash'])

    def run_actions(self, *names):
        tasks = [SimpleNamespace(name=name, action_args={'no_reset': True}) for name in names]
        original = list(tasks)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.callback(self.ctx, self.args, tasks)
        return tasks, original, output.getvalue()

    def cache(self, value):
        (self.build / 'CMakeCache.txt').write_text(f'ESP_VISION_ENABLE_IRISFW:BOOL={value}\n')

    def test_app_targets_work_for_ordinary_and_iris_builds(self):
        for board, iris in [(self.board, 'ON'), (self.board, 'OFF'), ('ESP32_P4X_EYE', None)]:
            for action in ('app', 'app-flash'):
                with self.subTest(board=board, iris=iris, action=action):
                    self.args.board = board
                    self.args.project_dir = str(self.root)
                    self.args.define_cache_entry = [] if iris is None else ['ESP_VISION_ENABLE_IRISFW=' + iris]
                    tasks, original, notice = self.run_actions(action)
                    self.assertIs(tasks[0], original[0])
                    self.assertEqual(self.args.build_dir, str(self.build))
                    self.assertEqual(self.args.project_dir,
                                     str(self.root / 'build/micropython/idf6.1/micropython/ports/esp32'))
                    self.assertIn('MICROPY_BOARD=' + board, self.args.define_cache_entry)
                    self.assertEqual(self.invoked, [])
                    self.assertEqual(notice, '')

    def test_iris_build_flash_monitor_selects_only_native_app_flash(self):
        self.args.define_cache_entry = ['ESP_VISION_ENABLE_IRISFW:BOOL=ON']
        tasks, original, notice = self.run_actions('all', 'flash', 'monitor')
        self.assertEqual([task.name for task in tasks], ['all', 'app-flash', 'monitor'])
        self.assertIs(tasks[0], original[0])
        self.assertIs(tasks[2], original[2])
        self.assertEqual(tasks[1].dependencies, ['app'])
        self.assertEqual(tasks[1].action_args, {'no_reset': True})
        self.assertIn('retaining Vibe Mode', notice)
        self.assertEqual(self.invoked, ['app-flash'])

    def test_cached_iris_option_without_repeated_define(self):
        self.cache('ON')
        tasks, _, _ = self.run_actions('flash')
        self.assertEqual(tasks[0].name, 'app-flash')

    def test_explicit_define_overrides_cache(self):
        for cached, explicit, expected in [('ON', 'OFF', 'flash'), ('OFF', 'ON', 'app-flash')]:
            with self.subTest(cached=cached, explicit=explicit):
                self.cache(cached)
                self.args.define_cache_entry = ['ESP_VISION_ENABLE_IRISFW:BOOL=' + explicit]
                tasks, _, _ = self.run_actions('flash')
                self.assertEqual(tasks[0].name, expected)

    def test_ordinary_flash_preserves_native_target_and_options(self):
        self.args.board = 'ESP32_P4X_EYE'
        tasks, original, notice = self.run_actions('all', 'flash', 'monitor')
        self.assertEqual(tasks, original)
        self.assertEqual(notice, '')
        self.assertEqual(self.invoked, [])

    def test_cached_cmake_false_values_do_not_enable_iris(self):
        for value in ('OFF', 'FALSE', 'NO', '0', '', 'N', 'IGNORE', 'NOTFOUND', 'x-NOTFOUND'):
            with self.subTest(value=value):
                self.cache(value)
                tasks, original, _ = self.run_actions('flash')
                self.assertIs(tasks[0], original[0])

    def test_app_target_is_allowed(self):
        tasks, _, _ = self.run_actions('app')
        self.assertEqual(tasks[0].name, 'app')

    def test_board_is_required_for_app_flash(self):
        self.args.board = None
        with self.assertRaisesRegex(RuntimeError, 'requires --board'):
            self.run_actions('app-flash')

    def test_unrelated_unsupported_action_stays_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Unsupported action'):
            self.run_actions('bootloader-flash')


if __name__ == '__main__':
    unittest.main()
