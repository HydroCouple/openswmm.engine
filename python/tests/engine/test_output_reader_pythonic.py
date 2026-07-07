"""
P5 — OutputReader Pythonic surface tests.

The fixture runs a small simulation in the conftest's ``completed_solver``
fixture, which writes a ``.out`` file. We re-open that file with the new
:class:`OutputReader` and exercise:

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
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._output_reader")

from openswmm.engine import (
    FileError,
    FlowUnits,
    OutLinkVar,
    OutNodeVar,
    OutSubcatchVar,
    OutSystemVar,
    OutputReader,
)


@pytest.fixture
def out_path(completed_solver, solver_files) -> str:
    """The .out file written by the completed_solver fixture.

    ``completed_solver`` runs the full simulation (producing the ``.out``
    file); ``solver_files`` carries the ``(inp, rpt, out)`` paths the solver
    was constructed with, so we read the ``.out`` path from there rather than
    from any (non-public) solver attribute.
    """
    _inp, _rpt, path = solver_files
    if not path or not os.path.exists(path):
        pytest.skip("completed_solver did not produce a .out file")
    return path


@pytest.fixture
def out(out_path) -> OutputReader:
    with OutputReader(out_path) as o:
        yield o


# ---------------------------------------------------------------------------
# Lifecycle + file-not-found
# ---------------------------------------------------------------------------


class TestLifecycle:
    def test_open_missing_file_raises(self, tmp_path):
        with pytest.raises(FileError):
            OutputReader(tmp_path / "definitely_not_here.out")

    def test_path_or_str(self, out_path):
        with OutputReader(Path(out_path)) as a, OutputReader(out_path) as b:
            assert a.period_count == b.period_count


# ---------------------------------------------------------------------------
# Metadata
# ---------------------------------------------------------------------------


class TestMetadata:
    def test_typed_metadata(self, out):
        assert isinstance(out.version, int)
        assert isinstance(out.flow_units, FlowUnits)
        assert isinstance(out.period_count, int) and out.period_count > 0
        assert isinstance(out.report_step, timedelta)
        assert isinstance(out.start_datetime, datetime)

    def test_object_id_lists(self, out):
        assert isinstance(out.node_ids, list)
        assert all(isinstance(s, str) for s in out.node_ids)
        assert len(out.node_ids) == out.node_count
        assert len(out.link_ids) == out.link_count
        assert len(out.subcatchment_ids) == out.subcatchment_count


# ---------------------------------------------------------------------------
# Time axis
# ---------------------------------------------------------------------------


class TestPeriodTimes:
    def test_period_times_is_datetime64_array(self, out):
        arr = out.period_times
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.dtype("datetime64[s]")
        assert arr.shape[0] == out.period_count

    def test_period_times_monotonic(self, out):
        arr = out.period_times
        diffs = np.diff(arr.astype("int64"))
        assert (diffs > 0).all()

    def test_period_times_cached(self, out):
        arr1 = out.period_times
        arr2 = out.period_times
        assert arr1 is arr2


# ---------------------------------------------------------------------------
# Per-period and time-series readers — enum + int|str
# ---------------------------------------------------------------------------


class TestResults:
    def test_node_result_returns_array(self, out):
        arr = out.node_result(0, OutNodeVar.DEPTH)
        assert isinstance(arr, np.ndarray)
        assert arr.shape[0] == out.node_count

    def test_link_result_returns_array(self, out):
        arr = out.link_result(0, OutLinkVar.FLOW)
        assert arr.shape[0] == out.link_count

    def test_subcatchment_result(self, out):
        arr = out.subcatchment_result(0, OutSubcatchVar.RUNOFF)
        assert arr.shape[0] == out.subcatchment_count

    def test_system_result(self, out):
        v = out.system_result(0, OutSystemVar.RUNOFF)
        assert isinstance(v, float)


class TestSeries:
    def test_node_series_str(self, out):
        if not out.node_ids:
            pytest.skip("no nodes")
        arr = out.node_series(out.node_ids[0], OutNodeVar.DEPTH)
        assert arr.shape[0] == out.period_count

    def test_node_series_int_matches_str(self, out):
        if not out.node_ids:
            pytest.skip("no nodes")
        a = out.node_series(0, OutNodeVar.DEPTH)
        b = out.node_series(out.node_ids[0], OutNodeVar.DEPTH)
        np.testing.assert_array_equal(a, b)

    def test_link_series(self, out):
        if not out.link_ids:
            pytest.skip("no links")
        arr = out.link_series(0, OutLinkVar.FLOW)
        assert arr.shape[0] == out.period_count

    def test_subcatchment_series(self, out):
        if not out.subcatchment_ids:
            pytest.skip("no subcatchments")
        arr = out.subcatchment_series(0, OutSubcatchVar.RUNOFF)
        assert arr.shape[0] == out.period_count

    def test_system_series(self, out):
        arr = out.system_series(OutSystemVar.RUNOFF)
        assert arr.shape[0] == out.period_count

    def test_range_kwargs(self, out):
        if out.period_count < 3:
            pytest.skip("need >= 3 periods")
        arr = out.node_series(0, OutNodeVar.DEPTH, start=1, end=2)
        assert arr.shape[0] == 2

    def test_unknown_id_raises_keyerror(self, out):
        with pytest.raises(KeyError):
            out.node_series("NO_SUCH_NODE_xyz", OutNodeVar.DEPTH)

    def test_bad_period_range(self, out):
        with pytest.raises(IndexError):
            out.node_series(0, OutNodeVar.DEPTH, start=-1, end=2)


# ---------------------------------------------------------------------------
# Attributes dict
# ---------------------------------------------------------------------------


class TestAttributes:
    def test_node_attributes_dict(self, out):
        if not out.node_ids:
            pytest.skip("no nodes")
        d = out.node_attributes(0, 0)
        assert isinstance(d, dict)
        # Base attributes are keyed by OutNodeVar; pollutant slots by int.
        for k in d:
            assert isinstance(k, (OutNodeVar, int))
        assert OutNodeVar.DEPTH in d

    def test_link_attributes_dict(self, out):
        if not out.link_ids:
            pytest.skip("no links")
        d = out.link_attributes(0, 0)
        assert OutLinkVar.FLOW in d

    def test_subcatchment_attributes_dict(self, out):
        if not out.subcatchment_ids:
            pytest.skip("no subcatchments")
        d = out.subcatchment_attributes(0, 0)
        assert OutSubcatchVar.RUNOFF in d


# ---------------------------------------------------------------------------
# Node stats sub-view
# ---------------------------------------------------------------------------


class TestNodeStats:
    def test_returns_view_with_typed_props(self, out):
        if not out.node_ids:
            pytest.skip("no nodes")
        stats = out.node_stats(0)
        for attr in ("max_depth", "max_overflow", "vol_flooded", "time_flooded"):
            assert isinstance(getattr(stats, attr), float)

    def test_str_and_int_agree(self, out):
        if not out.node_ids:
            pytest.skip("no nodes")
        a = out.node_stats(0).max_depth
        b = out.node_stats(out.node_ids[0]).max_depth
        assert a == b
