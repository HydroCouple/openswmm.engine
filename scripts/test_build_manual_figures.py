"""Regression checks for the manual figure cache and the run-decks summary.

Stub CLIs stand in for the engine. Fixtures and results are written to
tests/output/manual_figures_cache/<test>/ so they can be reviewed after a run.

    python -m unittest discover -s scripts -p test_build_manual_figures.py
"""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import shutil
import unittest

import build_manual_figures as bmf

OUTPUT = Path(__file__).resolve().parent.parent / "tests" / "output" / "manual_figures_cache"


class FixtureCase(unittest.TestCase):
    def setUp(self):
        self.root = OUTPUT / self.id().rsplit(".", 1)[-1]
        shutil.rmtree(self.root, ignore_errors=True)
        self.deck_dir = self.root / "figures" / "decks" / "demo"
        self.deck_dir.mkdir(parents=True)
        (self.deck_dir / "demo.inp").write_text("[OPTIONS]\nFLOW_UNITS CMS\n")
        (self.deck_dir / "rain.dat").write_text("rain 1\n")
        self.cli = self.root / "bin" / "openswmm"
        self.lib = self.root / "lib" / "libopenswmm.engine.6.dylib"
        self.lib.parent.mkdir(parents=True)
        self.lib.write_bytes(b"engine build 1")
        self.calls = self.root / "calls.log"
        self.fail_flag = self.root / "fail.flag"
        self.write_cli("1")

    def write_cli(self, variant, writes_results=True):
        """A CLI that logs each run, writes the sidecar into the report and exits 3 while fail.flag exists."""
        self.cli.parent.mkdir(parents=True, exist_ok=True)
        results = 'cat rain.dat > "$2"\necho out > "$3"\n' if writes_results else ""
        self.cli.write_text(f'#!/bin/sh\n# variant {variant}\necho "$@" >> "{self.calls}"\n'
                            f'{results}[ -f "{self.fail_flag}" ] && exit 3\nexit 0\n')
        self.cli.chmod(0o755)

    def run_deck(self, options=None, key="base"):
        sink = bmf.FigureSink(self.root / "figures", [], None, str(self.cli))
        with redirect_stdout(StringIO()):
            return sink.run("demo/demo.inp", options, key)

    def runs(self):
        return len(self.calls.read_text().splitlines()) if self.calls.exists() else 0


class FigureCacheTests(FixtureCase):
    def test_unchanged_engine_and_inputs_reuse_results(self):
        first = self.run_deck()
        second = self.run_deck()
        self.assertEqual(self.runs(), 1)
        self.assertEqual(second.seconds, 0.0)
        self.assertEqual(second.rpt.read_text(), first.rpt.read_text())

    def test_replaced_executable_reruns(self):
        self.run_deck()
        self.write_cli("2")
        self.run_deck()
        self.assertEqual(self.runs(), 2)

    def test_rebuilt_engine_library_reruns(self):
        self.run_deck()
        self.lib.write_bytes(b"engine build 2")
        self.run_deck()
        self.assertEqual(self.runs(), 2)

    def test_edited_sidecar_reruns_with_new_bytes(self):
        self.run_deck()
        (self.deck_dir / "rain.dat").write_text("rain 2\n")
        result = self.run_deck()
        self.assertEqual(self.runs(), 2)
        self.assertEqual(result.rpt.read_text(), "rain 2\n")

    def test_changed_options_rerun_under_the_same_key(self):
        self.run_deck({"FLOW_UNITS": "CMS"})
        self.run_deck({"FLOW_UNITS": "LPS"})
        self.assertEqual(self.runs(), 2)

    def test_failed_run_publishes_no_completion_record(self):
        self.fail_flag.write_text("")
        with self.assertRaises(RuntimeError):
            self.run_deck()
        work = self.root / "figures" / "cache" / "demo" / "base"
        self.assertTrue((work / "model.rpt").exists())  # the failed run's own report
        self.assertFalse((work / "cache.json").exists())
        # Same engine and inputs: only the missing record forces the rerun.
        self.fail_flag.unlink()
        self.run_deck()
        self.assertEqual(self.runs(), 2)
        self.assertTrue((work / "cache.json").exists())

    def test_success_without_results_publishes_no_completion_record(self):
        self.write_cli("silent", writes_results=False)
        with self.assertRaises(RuntimeError):
            self.run_deck()
        self.assertFalse((self.root / "figures" / "cache" / "demo" / "base" / "cache.json").exists())


class RunDecksSummaryTests(FixtureCase):
    def test_nonzero_exit_without_report_is_a_failure_note(self):
        self.cli.write_text("#!/bin/sh\nexit 5\n")
        out = self.root / "run_decks"
        printed = StringIO()
        with redirect_stdout(printed):
            status = bmf.main(["run-decks", "--cli", str(self.cli),
                               "--deck", str(self.deck_dir / "demo.inp"), "--out", str(out)])
        self.assertEqual(status, 1)
        self.assertIn("FAIL  demo.inp  exit=5", printed.getvalue())
        rows = (out / "summary.tsv").read_text().splitlines()
        self.assertEqual(rows[1].split("\t")[1], "5")
        self.assertEqual(rows[1].split("\t")[-1], "exit 5")


if __name__ == "__main__":
    unittest.main()
