"""
P7 — Pollutants / Tables / Inflows / Controls / Forcing Pythonic tests.

Combined into one module because each individual surface is small and
the contract checks are nearly identical: int|str, enums, container
protocol.
"""

from __future__ import annotations

from datetime import datetime

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._pollutants")

from openswmm.engine import (
    ConcentrationUnits,
    ForcingMode,
    ForcingTarget,
    PatternType,
    TableType,
)
from openswmm.engine._controls import Controls, ControlRule
from openswmm.engine._forcing import Forcing
from openswmm.engine._inflows import Inflows
from openswmm.engine._pollutants import Pollutant, Pollutants
from openswmm.engine._tables import Curve, Patterns, Pattern, Tables, TimeSeries


# ---------------------------------------------------------------------------
# Pollutants
# ---------------------------------------------------------------------------


class TestPollutants:
    def test_collection_protocol(self, opened_solver):
        coll = opened_solver.pollutants
        assert isinstance(coll, Pollutants)
        if len(coll) == 0:
            pytest.skip("fixture has no pollutants")
        first_id = coll.get_id(0)
        assert coll[first_id] == coll[0]

    def test_wrapper_props(self, opened_solver):
        if len(opened_solver.pollutants) == 0:
            pytest.skip("no pollutants")
        p = opened_solver.pollutants[0]
        assert isinstance(p.id, str)
        assert isinstance(p.units, ConcentrationUnits)
        assert isinstance(p.kdecay, float)
        p.kdecay = p.kdecay  # round-trip

    def test_co_pollutant(self, opened_solver):
        if len(opened_solver.pollutants) == 0:
            pytest.skip("no pollutants")
        p = opened_solver.pollutants[0]
        co = p.co_pollutant
        assert co is None or (isinstance(co[0], Pollutant) and isinstance(co[1], float))


# ---------------------------------------------------------------------------
# Tables / Patterns
# ---------------------------------------------------------------------------


class TestTables:
    def test_collection_protocol(self, opened_solver):
        coll = opened_solver.tables
        assert isinstance(coll, Tables)
        n = len(coll)
        assert n >= 0
        for t in coll:
            assert isinstance(t.id, str)

    def test_as_timeseries(self, opened_solver):
        if len(opened_solver.tables) == 0:
            pytest.skip("no tables")
        ts = opened_solver.tables.as_timeseries(0)
        assert isinstance(ts, TimeSeries)
        # ``points`` is a structured numpy array.
        pts = ts.points
        assert pts.dtype.names == ("time", "value")

    def test_as_curve(self, opened_solver):
        if len(opened_solver.tables) == 0:
            pytest.skip("no tables")
        c = opened_solver.tables.as_curve(0)
        assert isinstance(c, Curve)
        assert c.points.dtype == np.float64
        if len(c) > 0:
            assert c.points.shape[1] == 2

    def test_get_type(self, opened_solver):
        coll = opened_solver.tables
        if len(coll) == 0:
            pytest.skip("no tables")
        # Every table resolves to a TableType; index and id agree.
        for idx in range(len(coll)):
            t = coll.get_type(idx)
            assert isinstance(t, TableType)
            assert coll.get_type(coll.get_id(idx)) == t


class TestPatterns:
    def test_collection(self, opened_solver):
        coll = opened_solver.patterns
        assert isinstance(coll, Patterns)
        for p in coll:
            assert isinstance(p, Pattern)


# ---------------------------------------------------------------------------
# Inflows
# ---------------------------------------------------------------------------


class TestInflows:
    def test_counts_are_ints(self, opened_solver):
        inflows = opened_solver.inflows
        assert isinstance(inflows, Inflows)
        for attr in ("external_count", "dwf_count", "rdii_count",
                     "hydrograph_count", "hydrograph_gage_count",
                     "hydrograph_group_count", "rdii_decay_count"):
            v = getattr(inflows, attr)
            assert isinstance(v, int)
            assert v >= 0

    def test_add_external_int_or_str(self, opened_solver):
        if len(opened_solver.nodes) == 0:
            pytest.skip("no nodes")
        # Both call shapes must not raise.
        opened_solver.inflows.add_external(0, "FLOW")
        opened_solver.inflows.add_external(opened_solver.nodes.get_id(0), "FLOW")


# ---------------------------------------------------------------------------
# Controls
# ---------------------------------------------------------------------------


class TestControls:
    def test_view_type(self, opened_solver):
        assert isinstance(opened_solver.controls, Controls)

    def test_append_and_iterate(self, opened_solver):
        opened_solver.controls.clear()
        rule = (
            "RULE r1\n"
            "  IF SIMULATION TIME > 0\n"
            "  THEN PUMP P1 STATUS = OFF\n"
        )
        opened_solver.controls.append(rule)
        assert len(opened_solver.controls) == 1
        entry = opened_solver.controls[0]
        assert isinstance(entry, ControlRule)
        assert "PUMP" in entry.text or "rule" in entry.text.lower()

    def test_set_link_setting_int_or_str(self, opened_solver):
        if len(opened_solver.links) == 0:
            pytest.skip("no links")
        opened_solver.controls.set_link_setting(0, 0.5)
        lid = opened_solver.links.get_id(0)
        opened_solver.controls.set_link_setting(lid, 0.5)


# ---------------------------------------------------------------------------
# Forcing
# ---------------------------------------------------------------------------


class TestForcing:
    def test_view_type(self, running_solver):
        assert isinstance(running_solver.forcing, Forcing)

    def test_node_lat_inflow_int_or_str(self, running_solver):
        if len(running_solver.nodes) == 0:
            pytest.skip("no nodes")
        running_solver.forcing.node_lat_inflow(0, 0.1)
        running_solver.forcing.node_lat_inflow(
            running_solver.nodes.get_id(0), 0.2,
            mode=ForcingMode.ADD, persist=True)

    def test_clear_with_enum_target(self, running_solver):
        if len(running_solver.nodes) == 0:
            pytest.skip("no nodes")
        running_solver.forcing.node_lat_inflow(0, 0.1, persist=True)
        running_solver.forcing.clear(ForcingTarget.NODE, 0)

    def test_clear_all(self, running_solver):
        running_solver.forcing.clear_all()
