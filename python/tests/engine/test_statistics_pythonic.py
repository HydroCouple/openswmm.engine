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
    ])
    def test_bulk_property(self, completed_solver, prop, count_attr):
        arr = getattr(completed_solver.statistics, prop)
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(getattr(completed_solver, count_attr))
