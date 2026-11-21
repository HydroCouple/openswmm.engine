"""Regression checks for native release and Python development-wheel alignment."""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import check_version_sync as sync


class VersionSyncTests(unittest.TestCase):
    def check_versions(self, base, companion=None, pin=None, native="alpha.4"):
        companion = base if companion is None else companion
        pin = base if pin is None else pin
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "python").mkdir()
            (root / "packages/gpu-omp").mkdir(parents=True)
            (root / "CMakeLists.txt").write_text(
                f'project(openswmm VERSION 6.0.0)\nset(OPENSWMM_PRERELEASE "{native}")\n')
            suffix = f"-{native}" if native else ""
            (root / "vcpkg.json").write_text(
                '{"version-semver": "6.0.0' + suffix + '"}')
            (root / "python/pyproject.toml").write_text(f'version = "{base}"\n')
            (root / "packages/gpu-omp/pyproject.toml").write_text(
                f'version = "{companion}"\ndependencies = ["openswmm == {pin}"]\n')
            with patch.object(sync, "ROOT", root), redirect_stdout(StringIO()), redirect_stderr(StringIO()):
                return sync.main()

    def test_matching_development_builds(self):
        for version in ("6.0.0a4.dev1", "6.0.0a4.dev12"):
            with self.subTest(version=version):
                self.assertEqual(self.check_versions(version), 0)

    def test_completed_alpha_and_stable_releases(self):
        self.assertEqual(self.check_versions("6.0.0a4"), 0)
        self.assertEqual(self.check_versions("6.0.0", native=""), 0)

    def test_companion_must_match_development_number(self):
        self.assertEqual(self.check_versions("6.0.0a4.dev1", "6.0.0a4.dev2"), 1)

    def test_companion_cannot_drop_development_suffix(self):
        self.assertEqual(self.check_versions("6.0.0a4.dev1", "6.0.0a4"), 1)

    def test_dependency_pin_must_include_development_suffix(self):
        self.assertEqual(self.check_versions("6.0.0a4.dev1", pin="6.0.0a4"), 1)

    def test_python_still_must_match_native_release(self):
        self.assertEqual(self.check_versions("6.0.0a3.dev1"), 1)
        self.assertEqual(self.check_versions("6.0.0.dev1"), 1)

    def test_invalid_suffix_is_rejected(self):
        with self.assertRaises(ValueError):
            self.check_versions("6.0.0a4.devbanana")


if __name__ == "__main__":
    unittest.main()
