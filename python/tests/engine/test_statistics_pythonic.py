"""P6 — Statistics Pythonic surface tests."""

from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._statistics")

from openswmm.engine._statistics import Statistics


class TestStatisticsBulk:
    def test_view_returned(self, completed_solver):
        assert isinstance(completed_solver.statistics, Statistics)

    @pytest.mark.parametrize("prop, count_attr", [
        ("node_max_depth",        "nodes"),
        ("node_max_overflow",     "nodes"),
        ("node_vol_flooded",      "nodes"),
        ("node_time_flooded",     "nodes"),
        ("link_max_flow",         "links"),
        ("link_max_velocity",     "links"),
        ("link_max_filling",      "links"),
        ("link_vol_flow",         "links"),
        ("link_surcharge_time",   "links"),
        ("subcatchment_runoff_vol", "subcatchments"),
        ("subcatchment_max_runoff", "subcatchments"),
        ("subcatchment_precip", "subcatchments"),
    ])
    def test_bulk_property(self, completed_solver, prop, count_attr):
        arr = getattr(completed_solver.statistics, prop)
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(getattr(completed_solver, count_attr))


class TestScalarGetters:
    """P2.6 scalar getters must equal the bulk array at the same index."""

    @pytest.mark.parametrize("scalar, bulk", [
        ("node_max_depth_at", "node_max_depth"),
        ("node_max_overflow_at", "node_max_overflow"),
        ("node_vol_flooded_at", "node_vol_flooded"),
        ("node_time_flooded_at", "node_time_flooded"),
        ("link_max_flow_at", "link_max_flow"),
        ("link_max_velocity_at", "link_max_velocity"),
        ("link_max_filling_at", "link_max_filling"),
        ("link_surcharge_time_at", "link_surcharge_time"),
        ("link_vol_flow_at", "link_vol_flow"),
        ("subcatchment_max_runoff_at", "subcatchment_max_runoff"),
        ("subcatchment_runoff_vol_at", "subcatchment_runoff_vol"),
    ])
    def test_scalar_matches_bulk(self, completed_solver, scalar, bulk):
        stats = completed_solver.statistics
        arr = getattr(stats, bulk)
        if arr.shape[0] == 0:
            pytest.skip("no elements")
        getter = getattr(stats, scalar)
        for i in range(arr.shape[0]):
            assert getter(i) == pytest.approx(arr[i])


class TestSubcatchmentPrecip:
    """``subcatchment_precip`` is the only statistic with no C ``_bulk``
    companion; it is gathered scalar-wise. Verify it is reachable and that
    cumulative precipitation depths are physically sane (non-negative)."""

    def test_precip_non_negative(self, completed_solver):
        precip = completed_solver.statistics.subcatchment_precip
        assert precip.shape[0] == len(completed_solver.subcatchments)
        assert np.all(precip >= 0.0)
