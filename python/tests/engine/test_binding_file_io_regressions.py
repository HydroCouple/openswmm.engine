"""Binding-level regressions for authored text and teardown diagnostics."""

import unittest
import re
from pathlib import Path

from openswmm.engine import EngineError, ModelBuilder, Solver
from tests._paths import artifact_dir


class TestBindingFileIO(unittest.TestCase):
    def test_long_unicode_title_plugin_and_file_path(self):
        model = ModelBuilder()
        title = "Rainfall ☔ 雪 " + "á" * 5000
        plugin_args = "config=" + "x" * 6000
        file_path = "rain/" + "b" * 2400 + ".csv"
        model.add_title_line(title)
        model.plugin_set("example-plugin", plugin_args)
        model.set_file_path(1, file_path)

        self.assertEqual(model.get_title_line(0), title)
        self.assertEqual(model.plugin_get(0), ("example-plugin", plugin_args))
        self.assertEqual(model.get_file_path(1)[1], file_path)

        destination = artifact_dir(self) + "/long_text.inp"
        model.write(destination)
        with open(destination, encoding="utf-8") as saved:
            text = saved.read()
        self.assertIn(title, text)
        self.assertIn(plugin_args, text)

    def test_context_manager_reports_cleanup_failure_and_destroys(self):
        class FailingReport(Solver):
            def __init__(self):
                super().__init__()
                self.calls = []

            def end(self):
                self.calls.append("end")

            def report(self):
                self.calls.append("report")
                raise EngineError(10, "Cannot write report summary: disk failed")

            def close(self):
                self.calls.append("close")

            def destroy(self):
                self.calls.append("destroy")

        solver = FailingReport()
        with self.assertRaisesRegex(EngineError, "Cannot write report summary"):
            solver.__exit__(None, None, None)
        self.assertEqual(solver.calls, ["end", "report", "close", "destroy"])

        solver = FailingReport()
        body_error = ValueError("body failed")
        self.assertFalse(solver.__exit__(ValueError, body_error, None))
        self.assertEqual(solver.calls, ["end", "report", "close", "destroy"])
        self.assertTrue(
            any(
                "Cannot write report summary" in note
                for note in getattr(body_error, "__notes__", [])
            )
            or "Cannot write report summary" in str(body_error.__context__)
        )

    def test_failed_native_write_carries_its_destination(self):
        model = ModelBuilder()
        bad_path = artifact_dir(self) + "/missing_dir/failed.inp"
        with self.assertRaises(EngineError) as caught:
            model.write(bad_path)
        self.assertIn("failed.inp", str(caught.exception))

    def test_rain_and_snow_factors_affect_precipitation_once_and_persist(self):
        source = (
            Path(__file__).resolve().parents[3] / "tests/parity/snow/snow_parity.inp"
        )
        work = Path(artifact_dir(self))

        def precipitation(inp, rain, snow, tag, save=False):
            rpt = work / f"{tag}.rpt"
            solver = Solver(inp, rpt, work / f"{tag}.out")
            try:
                solver.open()
                gage = solver.gages["RG1"]
                gage.scale_factor = rain
                gage.snow_factor = snow
                if save:
                    solver.write(work / "snow_factors_saved.inp")
                solver.initialize()
                solver.start()
                while solver.step().total_seconds() > 0:
                    pass
                solver.end()
                solver.report()
            finally:
                solver.destroy()
            match = re.search(r"Total Precipitation \.{6,}\s+([\d.]+)", rpt.read_text())
            self.assertIsNotNone(match)
            return float(match.group(1))

        for rain, snow, expected in (
            (1, 1, 20.0),
            (10, 1, 200.0),
            (1, 1.4, 24.8),
            (10, 1.4, 248.0),
            (10, 10, 1280.0),
        ):
            with self.subTest(rain=rain, snow=snow):
                actual = precipitation(
                    source, rain, snow, f"rain{rain}_snow{snow}", rain == snow == 10
                )
                self.assertAlmostEqual(actual, expected, places=3)

        saved = work / "snow_factors_saved.inp"
        reopened = Solver(saved, work / "reopened.rpt", work / "reopened.out")
        try:
            reopened.open()
            self.assertEqual(reopened.gages["RG1"].scale_factor, 10)
            self.assertEqual(reopened.gages["RG1"].snow_factor, 10)
        finally:
            reopened.destroy()
        self.assertAlmostEqual(
            precipitation(saved, 10, 10, "reopened_run"), 1280.0, places=3
        )
