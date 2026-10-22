"""Binding-level regressions for authored text and teardown diagnostics."""

import unittest

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
            any("Cannot write report summary" in note for note in getattr(body_error, "__notes__", []))
            or "Cannot write report summary" in str(body_error.__context__)
        )

    def test_failed_native_write_carries_its_destination(self):
        model = ModelBuilder()
        bad_path = artifact_dir(self) + "/missing_dir/failed.inp"
        with self.assertRaises(EngineError) as caught:
            model.write(bad_path)
        self.assertIn("failed.inp", str(caught.exception))
