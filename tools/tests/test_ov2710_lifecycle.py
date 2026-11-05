# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run the adapted driver against an asynchronous FIFO timer daemon."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'tools/tests/fixtures/ov2710'
spec = importlib.util.spec_from_file_location('prepare_ov2710', ROOT / 'tools/prepare_ov2710.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


class OV2710LifecycleTests(unittest.TestCase):
    def test_source_identity_rejects_changed_driver(self):
        original = (FIXTURE / 'upstream.c').read_text()
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            adapter.prepare(original + '\n')
        self.assertEqual(adapter.prepare(original).split('*/', 1)[0], original.split('*/', 1)[0])

    def test_callback_and_failed_commands(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native C compiler unavailable')
        source = adapter.prepare((FIXTURE / 'upstream.c').read_text())
        parts = [(FIXTURE / 'daemon.c').read_text(),
                 function(source, 'struct ov2710_cam {') + ';',
                 (ROOT / 'platform/ov2710_lifecycle.inc').read_text(),
                 function(source, 'static void wb_timer_callback('),
                 function(source, 'static esp_err_t ov2710_set_stream('),
                 '#define free observe_free',
                 function(source, 'static esp_err_t ov2710_delete('),
                 '#undef free', (FIXTURE / 'exercise.c').read_text()]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'lifecycle.c'
            binary = Path(directory) / 'lifecycle'
            path.write_text('\n'.join(parts))
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     str(path), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
