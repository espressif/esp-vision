# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Link the production confirmation binding and recovery admission guards."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


class StorageRecoveryGuards(unittest.TestCase):
    def test_user_confirmation_ownership_handles_and_failure_cleanup(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")
        storage = (ROOT / "platform/storage.c").read_text()
        binding = (ROOT / "modules/py_storage.c").read_text()
        production = function(storage, "void esp_vision_storage_get_status(")
        production += "\n" + function(storage, "esp_err_t esp_vision_storage_format_flash(")
        production += "\n" + function(binding, "static mp_obj_t py_storage_format(")
        harness = (ROOT / "tools/tests/storage_recovery_guards_host_test.c").read_text()
        source = harness.replace("/* PRODUCTION_FUNCTIONS */", production)
        with tempfile.TemporaryDirectory(prefix="ev-storage-recovery-") as temporary:
            unit = Path(temporary) / "recovery.c"
            executable = Path(temporary) / "recovery"
            unit.write_text(source)
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'tools/tests/storage_writer_service_stubs'}",
                f"-I{ROOT / 'platform'}", str(unit), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
