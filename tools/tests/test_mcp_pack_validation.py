# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Keep the npm pack validator contract independent of deployment changes."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pack_validator', ROOT / 'tools/ci/mcp/validate_schema.py')
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class PackValidationTests(unittest.TestCase):
    def validate(self, pack, *options):
        cache = ROOT / '.cache' / 'pack-validation-tests'
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as directory:
            path = Path(directory) / 'pack.json'
            path.write_text(json.dumps(pack))
            schema = Path(directory) / 'schema.json'
            schema.write_text('{"type":"object"}')
            with mock.patch.object(validator, 'SCHEMA_PATH', schema), \
                    mock.patch.object(validator, '_semantic_checks', return_value=[]), \
                    mock.patch('sys.argv', ['validate_schema.py', str(path), *options]), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                return validator.main()

    def test_complete_snapshot_accepts_required_npm_arguments(self):
        pack = {'meta': {'version': '2026.10.21'}, 'modules': [
            {'source': 'micropython', 'symbols': [{'name': 'print'}]}]}
        self.assertEqual(self.validate(pack, '--require-micropython',
                                       '--expected-version', '2026.10.21'), 0)

    def test_requested_version_must_match(self):
        self.assertEqual(self.validate({'meta': {'version': 'old'}},
                                       '--expected-version', '2026.10.21'), 1)

    def test_missing_or_empty_micropython_knowledge_is_rejected(self):
        for modules in ([], [{'source': 'micropython', 'symbols': []}],
                        [{'source': 'esp-vision', 'symbols': [{'name': 'Image'}]}]):
            with self.subTest(modules=modules):
                self.assertEqual(self.validate({'modules': modules}, '--require-micropython'), 1)


if __name__ == '__main__':
    unittest.main()
