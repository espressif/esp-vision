# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Exercise SDK/firmware pipeline separation against the checked-in CI rules.

This intentionally models only the if/changes subset used by these jobs. GitLab
CI Lint remains authoritative for complete pipeline validation.
"""

import ast
import fnmatch
import subprocess
import re
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

try:
    import yaml
except ImportError:
    yaml = None

ROOT = Path(__file__).resolve().parents[2]
ZERO_SHA = "0" * 40


def condition(expression, variables):
    expression = re.sub(
        r'(\$\w+)\s*=~\s*/((?:\\.|[^/])*)/',
        lambda m: repr(bool(re.search(m[2], variables.get(m[1][1:], "")))),
        expression,
    )
    expression = re.sub(r'\$(\w+)', lambda m: repr(variables.get(m[1], "")), expression)
    return bool(eval(expression.replace("&&", " and ").replace("||", " or "),
                     {"__builtins__": {}}, {}))


def selected(rules, variables, changes, branch_changes=None):
    for rule in rules:
        if not condition(rule.get("if", "True"), variables):
            continue
        paths = rule.get("changes")
        changed = changes
        if isinstance(paths, dict):
            if paths["compare_to"] != "refs/heads/master":
                raise AssertionError("Unexpected comparison reference")
            changed = branch_changes if branch_changes is not None else changes
            paths = paths["paths"]
        if paths and not any(fnmatch.fnmatchcase(path, pattern.replace("**/*", "*"))
                             for path in changed for pattern in paths):
            continue
        return rule.get("when") != "never"
    return False


@unittest.skipIf(yaml is None, "PyYAML required (installed by evmux_host_checks)")
class DeviceCorePipelineRulesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rules = yaml.safe_load((ROOT / ".gitlab/ci/rules.yml").read_text())
        cls.core = yaml.safe_load((ROOT / ".gitlab/ci/device-core.yml").read_text())

    def variables(self, **overrides):
        return {
            "CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "feat/sdk",
            "CI_DEFAULT_BRANCH": "master", "CI_COMMIT_BEFORE_SHA": "1" * 40,
            "CI_COMMIT_REF_PROTECTED": "false", "BOARD": "ESP32_P4X_EYE",
            "IDF_IMAGE": "release-v5.5", **overrides,
        }

    def jobs(self, variables, changes, branch_changes=None):
        return {name: selected(job["rules"], variables, changes, branch_changes)
                for name, job in {
                    "sdk": self.core["device_core_checks"],
                    "publish": self.core["publish_device_core_npm"],
                    "firmware": self.rules[".rules:build:firmware"],
                    "hil": self.rules[".rules:test:target"],
                }.items()}

    def test_sdk_changes_on_master_do_not_build_or_flash(self):
        actual = self.jobs(self.variables(CI_COMMIT_BRANCH="master", CI_COMMIT_REF_PROTECTED="true"),
                           ["host/device-core/package.json"])
        self.assertEqual(actual, {"sdk": True, "publish": True, "firmware": False, "hil": False})

    def test_sdk_branch_and_merge_request_are_dry_run_only(self):
        for source in ("push", "merge_request_event"):
            with self.subTest(source=source):
                actual = self.jobs(self.variables(CI_PIPELINE_SOURCE=source),
                                   ["host/device-core/src/device.js"])
                self.assertEqual(actual, {"sdk": True, "publish": False, "firmware": False, "hil": False})

    def test_core_only_api_overrides_firmware_changes_and_explicit_matrix(self):
        actual = self.jobs(self.variables(CI_PIPELINE_SOURCE="api", ESP_VISION_DEVICE_CORE_ONLY="1",
                                          ESP_VISION_BUILD_FIRMWARE="1"), ["platform/main.c"])
        self.assertEqual(actual, {"sdk": True, "publish": False, "firmware": False, "hil": False})

    def test_sdk_tag_never_builds_or_flashes_or_publishes(self):
        actual = self.jobs(self.variables(CI_COMMIT_BRANCH="", CI_COMMIT_TAG="device-core-v0.1.0"),
                           ["platform/main.c", "host/device-core/package.json"])
        self.assertEqual(actual, {"sdk": True, "publish": False, "firmware": False, "hil": False})

    def test_firmware_tags_keep_the_matrix_and_hil(self):
        for tag in ("v1.0.0", "2026.10.15"):
            actual = self.jobs(self.variables(CI_COMMIT_BRANCH="", CI_COMMIT_TAG=tag), [])
            self.assertTrue(actual["firmware"])
            self.assertTrue(actual["hil"])
            self.assertFalse(actual["publish"])

    def test_sdk_ci_changes_only_run_sdk_checks(self):
        actual = self.jobs(self.variables(), [".gitlab/ci/device-core.yml"])
        self.assertEqual(actual, {"sdk": True, "publish": False, "firmware": False, "hil": False})

    def test_push_does_not_fall_through_to_whole_branch_diff(self):
        actual = self.jobs(self.variables(), ["host/device-core/src/device.js"], ["platform/main.c"])
        self.assertFalse(actual["firmware"])
        self.assertFalse(actual["hil"])

    def test_new_branch_and_web_use_explicit_comparison(self):
        for overrides in ({"CI_COMMIT_BEFORE_SHA": ZERO_SHA}, {"CI_PIPELINE_SOURCE": "web"},
                          {"CI_PIPELINE_SOURCE": "api"}):
            with self.subTest(overrides=overrides):
                actual = self.jobs(self.variables(**overrides), ["platform/main.c"],
                                   ["host/device-core/src/device.js"])
                self.assertEqual(actual, {"sdk": True, "publish": False, "firmware": False, "hil": False})

    def test_firmware_changes_keep_push_and_mr_hil_in_sync(self):
        for source in ("push", "merge_request_event"):
            for branch in ("master", "feat/firmware"):
                actual = self.jobs(self.variables(CI_PIPELINE_SOURCE=source, CI_COMMIT_BRANCH=branch),
                                   ["platform/main.c"])
                self.assertTrue(actual["firmware"])
                self.assertTrue(actual["hil"])

    def test_targeted_bin_omits_other_boards_and_hil(self):
        variables = self.variables(ESP_VISION_TEST_BOARD="ESP32_P4X_EYE",
                                   ESP_VISION_TEST_IDF_IMAGE="release-v5.5")
        actual = self.jobs(variables, [])
        self.assertTrue(actual["firmware"])
        self.assertFalse(actual["hil"])
        for mismatch in ({"BOARD": "ESP32_P4X_VISION"}, {"IDF_IMAGE": "release-v6.1"}):
            self.assertFalse(self.jobs({**variables, **mismatch}, ["platform/main.c"])["firmware"])

    def test_explicit_final_matrix_remains_available(self):
        actual = self.jobs(self.variables(CI_PIPELINE_SOURCE="api", ESP_VISION_BUILD_FIRMWARE="1"), [])
        self.assertTrue(actual["firmware"])
        self.assertTrue(actual["hil"])

    def test_publication_requires_protected_default_branch_push_and_sdk_changes(self):
        for overrides, paths in (
            ({"CI_COMMIT_BRANCH": "master"}, ["host/device-core/package.json"]),
            ({"CI_COMMIT_REF_PROTECTED": "true"}, ["host/device-core/package.json"]),
            ({"CI_COMMIT_BRANCH": "master", "CI_COMMIT_REF_PROTECTED": "true",
              "CI_PIPELINE_SOURCE": "web"}, ["host/device-core/package.json"]),
            ({"CI_COMMIT_BRANCH": "master", "CI_COMMIT_REF_PROTECTED": "true"}, [".gitlab/ci/device-core.yml"]),
        ):
            self.assertFalse(self.jobs(self.variables(**overrides), paths)["publish"])

    def test_checks_validate_isolated_package_and_release_dry_run(self):
        job = self.core["device_core_checks"]
        self.assertEqual(job["image"], "node:22-bookworm")
        self.assertEqual(job["variables"]["GIT_SUBMODULE_STRATEGY"], "none")
        self.assertIn("cd host/device-core", job["script"])
        install = "npm install --ignore-scripts --no-audit --no-fund --package-lock=false --registry=https://registry.npmjs.org/"
        self.assertLess(job["script"].index(install), job["script"].index("npm test"))
        for command in ("npm test", "npm run check:schema", "npm run check:package",
                        "node scripts/release-npm.mjs --dry-run"):
            self.assertIn(command, job["script"])
        self.assertEqual(self.core["publish_device_core_npm"]["needs"], ["device_core_checks"])


class LaunchpadSdkTagIsolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Exercise the actual tag-selection function without importing optional
        # build-only rtoml / board-model dependencies into the host check job.
        source = ROOT / "tools/ci/launchpad/generate_launchpad.py"
        tree = ast.parse(source.read_text(), filename=str(source))
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == "collect_release_tags")
        namespace = {"subprocess": subprocess, "REPO_ROOT": str(ROOT)}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
        cls.collect = staticmethod(namespace["collect_release_tags"])

    def test_sdk_tags_are_excluded_without_narrowing_historical_firmware_tags(self):
        history = "device-core-v0.2.0\n2026.10.15\nv1.2.3-rc1\nlegacy-release\ndevice-core-v0.1.0\n"
        with patch.object(subprocess, "run", return_value=SimpleNamespace(stdout=history)):
            self.assertEqual(self.collect("2026.10.15"),
                             ["2026.10.15", "v1.2.3-rc1", "legacy-release"])

    def test_sdk_tag_dry_run_does_not_seed_a_fake_firmware_release(self):
        with patch.object(subprocess, "run", return_value=SimpleNamespace(
                stdout="device-core-v0.1.0\nv1.0.0\n2026.08.27\n")):
            self.assertEqual(self.collect("device-core-v0.2.0"), ["v1.0.0", "2026.08.27"])

    def test_unpublished_firmware_dry_run_still_seeds_current_tag(self):
        with patch.object(subprocess, "run", return_value=SimpleNamespace(stdout="v1.0.0\n")):
            self.assertEqual(self.collect("v0.0.0-dev"), ["v0.0.0-dev", "v1.0.0"])

    def test_missing_git_keeps_firmware_seed_but_never_adds_sdk_seed(self):
        with patch.object(subprocess, "run", side_effect=FileNotFoundError):
            self.assertEqual(self.collect("v0.0.0-dev"), ["v0.0.0-dev"])
            self.assertEqual(self.collect("device-core-v0.1.0"), [])


if __name__ == "__main__":
    unittest.main()
