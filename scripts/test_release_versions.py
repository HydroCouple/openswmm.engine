"""bump_version.py, the version gate and python_release_version.py on the real declarations.

Each test copies the files that declare the version into its own folder under
tests/output/version_scripts/, kept for review and replaced on the next run.

    python -m unittest discover -s scripts -p "test_*version*.py"
"""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import shutil
import unittest
from unittest.mock import patch

import bump_version
import check_version_sync as sync
import python_release_version as release

OUT = sync.ROOT / "tests" / "output" / "version_scripts"
FILES = sorted({name for name, _, _ in sync.DECLARATIONS})


def tree(name: str, version: str | None = None) -> Path:
    """A copy of the declaring files, bumped to version when one is given."""
    root = OUT / name
    shutil.rmtree(root, ignore_errors=True)
    for f in FILES:
        (root / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sync.ROOT / f, root / f)
    if version is not None:
        bump_version.bump(version, root)
    return root


def gate(root: Path) -> int:
    with patch.object(sync, "ROOT", root), redirect_stdout(StringIO()), \
            redirect_stderr(StringIO()):
        return sync.main()


def values(root: Path) -> list[tuple[str, str, str]]:
    return [(name, form, sync.find(sync.read(root / name), name, pattern).group(1))
            for name, pattern, form in sync.DECLARATIONS]


class BumpTests(unittest.TestCase):
    def test_the_repository_passes_the_gate(self):
        self.assertEqual(gate(sync.ROOT), 0)

    def test_a_bump_writes_every_declaration_and_passes_the_gate(self):
        root = tree("bump_beta", "6.1.0-beta.2")
        expected = {"base": "6.1.0", "prerelease": "beta.2", "semver": "6.1.0-beta.2",
                    "python": "6.1.0b2"}
        for name, form, value in values(root):
            with self.subTest(file=name, form=form):
                self.assertEqual(value, expected[form])
        self.assertEqual(gate(root), 0)

    def test_a_final_release_has_no_prerelease(self):
        root = tree("bump_final", "6.1.0")
        for name, form, value in values(root):
            with self.subTest(file=name, form=form):
                self.assertEqual(value, "" if form == "prerelease" else "6.1.0")
        self.assertEqual(gate(root), 0)

    def test_a_bump_and_back_leaves_every_file_as_it_was(self):
        source = sync.release_forms(*sync.from_cmake(sync.ROOT / "CMakeLists.txt"))
        root = tree("bump_round_trip", "6.1.0-rc.3")
        bump_version.bump(source["semver"], root)
        # A committed .devN comes back in its release form; everything else byte for byte.
        dev = sync.python_version(sync.ROOT / "python" / "pyproject.toml") != source["pep440"]
        for f in FILES:
            if dev and (f.endswith("pyproject.toml") or f == "python/docs/conf.py"):
                continue
            with self.subTest(file=f):
                self.assertEqual((root / f).read_bytes(), (sync.ROOT / f).read_bytes())

    def test_a_version_it_cannot_spell_is_refused_and_nothing_is_written(self):
        root = tree("bump_refused")
        before = {f: (root / f).read_bytes() for f in FILES}
        for bad in ("6.1.0a1", "6.1", "6.1.0-gamma.1", "6.1.0-alpha", "v6.1.0"):
            with self.subTest(version=bad), self.assertRaises(ValueError):
                bump_version.bump(bad, root)
        self.assertEqual({f: (root / f).read_bytes() for f in FILES}, before)

    def test_every_restated_declaration_is_gated(self):
        for i, (name, pattern, form) in enumerate(sync.RESTATED):
            with self.subTest(file=name, pattern=pattern):
                root = tree(f"drift_{i}")
                text = sync.read(root / name)
                sync.write(root / name, sync.replace(text, name, pattern, "9.9.9"))
                self.assertEqual(gate(root), 1)

    def test_a_declaration_written_twice_fails_the_gate(self):
        root = tree("written_twice")
        text = sync.read(root / "CITATION.cff")
        line = sync.find(text, "CITATION.cff", r'^version:\s*"[^"]+"\n').group(0)
        sync.write(root / "CITATION.cff", text + line)
        self.assertEqual(gate(root), 1)

    def test_a_missing_declaring_file_fails_the_gate(self):
        root = tree("missing_file")
        (root / "CITATION.cff").unlink()
        self.assertEqual(gate(root), 1)


class ResolveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tree("resolve", "6.2.0-rc.3")

    def resolve(self, ref, dev_run=None):
        return release.resolve(ref, dev_run, self.root)

    def test_an_engine_release_tag_builds_its_pep440_form(self):
        self.assertEqual(self.resolve("refs/tags/v6.2.0-rc.3"), "6.2.0rc3")

    def test_an_engine_release_tag_must_equal_the_source(self):
        for ref in ("refs/tags/v6.2.0-rc.2", "refs/tags/v6.2.0", "refs/tags/v6.2.0rc3"):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                self.resolve(ref)

    def test_a_python_only_tag_carries_a_post_or_dev_suffix(self):
        for version in ("6.2.0rc3.post1", "6.2.0rc3.dev4", "6.2.0rc3.post1.dev2"):
            with self.subTest(version=version):
                self.assertEqual(self.resolve(f"refs/tags/py-v{version}"), version)

    def test_a_python_only_tag_that_does_not_fit_is_refused(self):
        for version in ("6.2.0rc3", "6.2.0rc2.post1", "6.2.0rc3.dev", "6.2.0rc3-post1",
                        "6.2.0-rc.3.post1", "6.2.0rc3.dev1.post2", "6.2.0rc3.post1 "):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.resolve(f"refs/tags/py-v{version}")

    def test_development_wheels_are_numbered_by_the_run(self):
        self.assertEqual(self.resolve("refs/pull/135/merge", 355), "6.2.0rc3.dev355")

    def test_other_refs_keep_the_committed_version(self):
        for ref in ("refs/heads/swmm6_rel", "refs/pull/7/merge", "refs/heads/main"):
            with self.subTest(ref=ref):
                self.assertEqual(self.resolve(ref), "")

    def test_the_command_line_names_a_bad_tag_and_exits_1(self):
        err = StringIO()
        with patch.object(sync, "ROOT", self.root), redirect_stdout(StringIO()), \
                redirect_stderr(err):
            code = release.main(["resolve", "--ref", "refs/tags/v9.9.9"])
        self.assertEqual(code, 1)
        self.assertIn("does not match CMakeLists.txt's version 6.2.0-rc.3", err.getvalue())


class StampTests(unittest.TestCase):
    def packages(self, root):
        """The two package versions and the companion's pin (the manual's fallback is not
        stamped)."""
        return [value for name, form, value in values(root)
                if form == "python" and name != "python/docs/conf.py"]

    def test_stamp_writes_both_packages_and_the_pin(self):
        root = tree("stamp_dev", "6.2.0-rc.3")
        release.stamp("6.2.0rc3.dev355", root)
        self.assertEqual(self.packages(root), ["6.2.0rc3.dev355"] * 3)

    def test_stamp_takes_a_post_release(self):
        root = tree("stamp_post", "6.2.0-rc.3")
        release.stamp("6.2.0rc3.post1", root)
        self.assertEqual(self.packages(root), ["6.2.0rc3.post1"] * 3)

    def test_stamp_refuses_another_release_and_writes_nothing(self):
        root = tree("stamp_refused", "6.2.0-rc.3")
        before = {f: (root / f).read_bytes() for f in FILES}
        for bad in ("6.2.0rc2.dev1", "6.2.1", "6.2.0rc3.devx", "6.2.0rc3+local"):
            with self.subTest(version=bad), self.assertRaises(ValueError):
                release.stamp(bad, root)
        self.assertEqual({f: (root / f).read_bytes() for f in FILES}, before)


if __name__ == "__main__":
    unittest.main()
