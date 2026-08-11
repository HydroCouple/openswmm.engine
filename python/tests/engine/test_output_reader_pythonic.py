"""
P5 — OutputReader Pythonic surface tests.

The ``completed_solver`` helper runs a small simulation which writes a
``.out`` file. We re-open that file with the new :class:`OutputReader`
and exercise:

* Metadata properties: enum-typed flow_units, datetime/timedelta types.
* Object id arrays (``node_ids`` etc.) — lists of str, length-matched.
* ``period_times`` numpy array (datetime64[s]).
* ``*_result(period, var=Enum)`` and ``*_series(key, var=Enum)`` with
  ``int`` and ``str`` object selectors.
* ``*_attributes(key, period)`` returns ``Dict[Enum, float]``.
* ``node_stats(key)`` returns a typed view.
* Errors: ``OutputReader("missing.out")`` raises :class:`FileError`.
"""

from __future__ import annotations

import os
import unittest
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

try:
    import openswmm.engine._output_reader  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (
    FileError,
    FlowUnits,
    OutLinkVar,
    OutNodeVar,
    OutSubcatchVar,
    OutSystemVar,
    OutputReader,
)

from tests._paths import artifact_dir
from tests.engine._solver_cases import EngineSolverCase


class OutputReaderCase(EngineSolverCase):
    """Base case providing the .out file and an open OutputReader."""

    def out_path(self) -> str:
        """The .out file written by the completed_solver helper.

        ``completed_solver`` runs the full simulation (producing the ``.out``
        file); ``solver_files`` carries the ``(inp, rpt, out)`` paths the solver
        was constructed with, so we read the ``.out`` path from there rather than
        from any (non-public) solver attribute.
        """
        self.completed_solver()
        _inp, _rpt, path = self.solver_files()
        if not path or not os.path.exists(path):
            self.skipTest("completed_solver did not produce a .out file")
        return path

    def out(self) -> OutputReader:
        o = OutputReader(self.out_path())
        self.addCleanup(o.close)
        return o


# ---------------------------------------------------------------------------
# Lifecycle + file-not-found
# ---------------------------------------------------------------------------


class TestLifecycle(OutputReaderCase):
    def test_open_missing_file_raises(self):
        d = artifact_dir(self)
        with self.assertRaises(FileError):
            OutputReader(os.path.join(d, "definitely_not_here.out"))

    def test_path_or_str(self):
        out_path = self.out_path()
        with OutputReader(Path(out_path)) as a, OutputReader(out_path) as b:
            self.assertEqual(a.period_count, b.period_count)


# ---------------------------------------------------------------------------
# Metadata
# ---------------------------------------------------------------------------


class TestMetadata(OutputReaderCase):
    def test_typed_metadata(self):
        out = self.out()
        self.assertIsInstance(out.version, int)
        self.assertIsInstance(out.flow_units, FlowUnits)
        self.assertIsInstance(out.period_count, int)
        self.assertGreater(out.period_count, 0)
        self.assertIsInstance(out.report_step, timedelta)
        self.assertIsInstance(out.start_datetime, datetime)

    def test_object_id_lists(self):
        out = self.out()
        self.assertIsInstance(out.node_ids, list)
        self.assertTrue(all(isinstance(s, str) for s in out.node_ids))
        self.assertEqual(len(out.node_ids), out.node_count)
        self.assertEqual(len(out.link_ids), out.link_count)
        self.assertEqual(len(out.subcatchment_ids), out.subcatchment_count)


# ---------------------------------------------------------------------------
# Time axis
# ---------------------------------------------------------------------------


class TestPeriodTimes(OutputReaderCase):
    def test_period_times_is_datetime64_array(self):
        out = self.out()
        arr = out.period_times
        self.assertIsInstance(arr, np.ndarray)
        self.assertEqual(arr.dtype, np.dtype("datetime64[s]"))
        self.assertEqual(arr.shape[0], out.period_count)

    def test_period_times_monotonic(self):
        out = self.out()
        arr = out.period_times
        diffs = np.diff(arr.astype("int64"))
        self.assertTrue((diffs > 0).all())

    def test_period_times_cached(self):
        out = self.out()
        arr1 = out.period_times
        arr2 = out.period_times
        self.assertIs(arr1, arr2)


# ---------------------------------------------------------------------------
# Per-period and time-series readers — enum + int|str
# ---------------------------------------------------------------------------


class TestResults(OutputReaderCase):
    def test_node_result_returns_array(self):
        out = self.out()
        arr = out.node_result(0, OutNodeVar.DEPTH)
        self.assertIsInstance(arr, np.ndarray)
        self.assertEqual(arr.shape[0], out.node_count)

    def test_link_result_returns_array(self):
        out = self.out()
        arr = out.link_result(0, OutLinkVar.FLOW)
        self.assertEqual(arr.shape[0], out.link_count)

    def test_subcatchment_result(self):
        out = self.out()
        arr = out.subcatchment_result(0, OutSubcatchVar.RUNOFF)
        self.assertEqual(arr.shape[0], out.subcatchment_count)

    def test_system_result(self):
        out = self.out()
        v = out.system_result(0, OutSystemVar.RUNOFF)
        self.assertIsInstance(v, float)


class TestSeries(OutputReaderCase):
    def test_node_series_str(self):
        out = self.out()
        if not out.node_ids:
            self.skipTest("no nodes")
        arr = out.node_series(out.node_ids[0], OutNodeVar.DEPTH)
        self.assertEqual(arr.shape[0], out.period_count)

    def test_node_series_int_matches_str(self):
        out = self.out()
        if not out.node_ids:
            self.skipTest("no nodes")
        a = out.node_series(0, OutNodeVar.DEPTH)
        b = out.node_series(out.node_ids[0], OutNodeVar.DEPTH)
        np.testing.assert_array_equal(a, b)

    def test_link_series(self):
        out = self.out()
        if not out.link_ids:
            self.skipTest("no links")
        arr = out.link_series(0, OutLinkVar.FLOW)
        self.assertEqual(arr.shape[0], out.period_count)

    def test_subcatchment_series(self):
        out = self.out()
        if not out.subcatchment_ids:
            self.skipTest("no subcatchments")
        arr = out.subcatchment_series(0, OutSubcatchVar.RUNOFF)
        self.assertEqual(arr.shape[0], out.period_count)

    def test_system_series(self):
        out = self.out()
        arr = out.system_series(OutSystemVar.RUNOFF)
        self.assertEqual(arr.shape[0], out.period_count)

    def test_range_kwargs(self):
        out = self.out()
        if out.period_count < 3:
            self.skipTest("need >= 3 periods")
        arr = out.node_series(0, OutNodeVar.DEPTH, start=1, end=2)
        self.assertEqual(arr.shape[0], 2)

    def test_unknown_id_raises_keyerror(self):
        out = self.out()
        with self.assertRaises(KeyError):
            out.node_series("NO_SUCH_NODE_xyz", OutNodeVar.DEPTH)

    def test_bad_period_range(self):
        out = self.out()
        with self.assertRaises(IndexError):
            out.node_series(0, OutNodeVar.DEPTH, start=-1, end=2)


# ---------------------------------------------------------------------------
# Attributes dict
# ---------------------------------------------------------------------------


class TestAttributes(OutputReaderCase):
    def test_node_attributes_dict(self):
        out = self.out()
        if not out.node_ids:
            self.skipTest("no nodes")
        d = out.node_attributes(0, 0)
        self.assertIsInstance(d, dict)
        # Base attributes are keyed by OutNodeVar; pollutant slots by int.
        for k in d:
            self.assertIsInstance(k, (OutNodeVar, int))
        self.assertIn(OutNodeVar.DEPTH, d)

    def test_link_attributes_dict(self):
        out = self.out()
        if not out.link_ids:
            self.skipTest("no links")
        d = out.link_attributes(0, 0)
        self.assertIn(OutLinkVar.FLOW, d)

    def test_subcatchment_attributes_dict(self):
        out = self.out()
        if not out.subcatchment_ids:
            self.skipTest("no subcatchments")
        d = out.subcatchment_attributes(0, 0)
        self.assertIn(OutSubcatchVar.RUNOFF, d)


# ---------------------------------------------------------------------------
# Node stats sub-view
# ---------------------------------------------------------------------------


class TestNodeStats(OutputReaderCase):
    def test_returns_view_with_typed_props(self):
        out = self.out()
        if not out.node_ids:
            self.skipTest("no nodes")
        stats = out.node_stats(0)
        for attr in ("max_depth", "max_overflow", "vol_flooded", "time_flooded"):
            self.assertIsInstance(getattr(stats, attr), float)

    def test_str_and_int_agree(self):
        out = self.out()
        if not out.node_ids:
            self.skipTest("no nodes")
        a = out.node_stats(0).max_depth
        b = out.node_stats(out.node_ids[0]).max_depth
        self.assertEqual(a, b)
