"""
P7 — Pollutants / Tables / Inflows / Controls / Forcing Pythonic tests.

Combined into one module because each individual surface is small and
the contract checks are nearly identical: int|str, enums, container
protocol.
"""

from __future__ import annotations

import unittest

from datetime import datetime

import numpy as np

try:
    import openswmm.engine._pollutants  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (  # noqa: E402
    ConcentrationUnits,
    ForcingMode,
    ForcingTarget,
    PatternType,
    TableType,
)
from openswmm.engine._controls import Controls, ControlRule  # noqa: E402
from openswmm.engine._forcing import Forcing  # noqa: E402
from openswmm.engine._inflows import Inflows  # noqa: E402
from openswmm.engine._pollutants import Pollutant, Pollutants  # noqa: E402
from openswmm.engine._tables import Curve, Patterns, Pattern, Tables, TimeSeries  # noqa: E402

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


# ---------------------------------------------------------------------------
# Pollutants
# ---------------------------------------------------------------------------


class TestPollutants(EngineSolverCase):
    def test_collection_protocol(self):
        opened_solver = self.opened_solver()
        coll = opened_solver.pollutants
        self.assertIsInstance(coll, Pollutants)
        if len(coll) == 0:
            self.skipTest("fixture has no pollutants")
        first_id = coll.get_id(0)
        self.assertEqual(coll[first_id], coll[0])

    def test_wrapper_props(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.pollutants) == 0:
            self.skipTest("no pollutants")
        p = opened_solver.pollutants[0]
        self.assertIsInstance(p.id, str)
        self.assertIsInstance(p.units, ConcentrationUnits)
        self.assertIsInstance(p.kdecay, float)
        p.kdecay = p.kdecay  # round-trip

    def test_co_pollutant(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.pollutants) == 0:
            self.skipTest("no pollutants")
        p = opened_solver.pollutants[0]
        co = p.co_pollutant
        self.assertTrue(
            co is None or (isinstance(co[0], Pollutant) and isinstance(co[1], float)))


# ---------------------------------------------------------------------------
# Tables / Patterns
# ---------------------------------------------------------------------------


class TestTables(EngineSolverCase):
    def test_collection_protocol(self):
        opened_solver = self.opened_solver()
        coll = opened_solver.tables
        self.assertIsInstance(coll, Tables)
        n = len(coll)
        self.assertGreaterEqual(n, 0)
        for t in coll:
            self.assertIsInstance(t.id, str)

    def test_as_timeseries(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.tables) == 0:
            self.skipTest("no tables")
        ts = opened_solver.tables.as_timeseries(0)
        self.assertIsInstance(ts, TimeSeries)
        # ``points`` is a structured numpy array.
        pts = ts.points
        self.assertEqual(pts.dtype.names, ("time", "value"))

    def test_as_curve(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.tables) == 0:
            self.skipTest("no tables")
        c = opened_solver.tables.as_curve(0)
        self.assertIsInstance(c, Curve)
        self.assertEqual(c.points.dtype, np.float64)
        if len(c) > 0:
            self.assertEqual(c.points.shape[1], 2)

    def test_get_type(self):
        opened_solver = self.opened_solver()
        coll = opened_solver.tables
        if len(coll) == 0:
            self.skipTest("no tables")
        # Every table resolves to a TableType; index and id agree.
        for idx in range(len(coll)):
            t = coll.get_type(idx)
            self.assertIsInstance(t, TableType)
            self.assertEqual(coll.get_type(coll.get_id(idx)), t)


class TestPatterns(EngineSolverCase):
    def test_collection(self):
        opened_solver = self.opened_solver()
        coll = opened_solver.patterns
        self.assertIsInstance(coll, Patterns)
        for p in coll:
            self.assertIsInstance(p, Pattern)


# ---------------------------------------------------------------------------
# Inflows
# ---------------------------------------------------------------------------


class TestInflows(EngineSolverCase):
    def test_counts_are_ints(self):
        opened_solver = self.opened_solver()
        inflows = opened_solver.inflows
        self.assertIsInstance(inflows, Inflows)
        for attr in ("external_count", "dwf_count", "rdii_count",
                     "hydrograph_count", "hydrograph_gage_count",
                     "hydrograph_group_count", "rdii_decay_count"):
            v = getattr(inflows, attr)
            self.assertIsInstance(v, int)
            self.assertGreaterEqual(v, 0)

    def test_add_external_int_or_str(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.nodes) == 0:
            self.skipTest("no nodes")
        # Both call shapes must not raise.
        opened_solver.inflows.add_external(0, "FLOW")
        opened_solver.inflows.add_external(opened_solver.nodes.get_id(0), "FLOW")


# ---------------------------------------------------------------------------
# Controls
# ---------------------------------------------------------------------------


class TestControls(EngineSolverCase):
    def test_view_type(self):
        opened_solver = self.opened_solver()
        self.assertIsInstance(opened_solver.controls, Controls)

    def test_append_and_iterate(self):
        opened_solver = self.opened_solver()
        opened_solver.controls.clear()
        rule = (
            "RULE r1\n"
            "  IF SIMULATION TIME > 0\n"
            "  THEN PUMP P1 STATUS = OFF\n"
        )
        opened_solver.controls.append(rule)
        self.assertEqual(len(opened_solver.controls), 1)
        entry = opened_solver.controls[0]
        self.assertIsInstance(entry, ControlRule)
        self.assertTrue("PUMP" in entry.text or "rule" in entry.text.lower())

    def test_set_link_setting_int_or_str(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.links) == 0:
            self.skipTest("no links")
        opened_solver.controls.set_link_setting(0, 0.5)
        lid = opened_solver.links.get_id(0)
        opened_solver.controls.set_link_setting(lid, 0.5)


# ---------------------------------------------------------------------------
# Forcing
# ---------------------------------------------------------------------------


class TestForcing(EngineSolverCase):
    def test_view_type(self):
        running_solver = self.running_solver()
        self.assertIsInstance(running_solver.forcing, Forcing)

    def test_node_lat_inflow_int_or_str(self):
        running_solver = self.running_solver()
        if len(running_solver.nodes) == 0:
            self.skipTest("no nodes")
        running_solver.forcing.node_lat_inflow(0, 0.1)
        running_solver.forcing.node_lat_inflow(
            running_solver.nodes.get_id(0), 0.2,
            mode=ForcingMode.ADD, persist=True)

    def test_clear_with_enum_target(self):
        running_solver = self.running_solver()
        if len(running_solver.nodes) == 0:
            self.skipTest("no nodes")
        running_solver.forcing.node_lat_inflow(0, 0.1, persist=True)
        running_solver.forcing.clear(ForcingTarget.NODE, 0)

    def test_clear_all(self):
        running_solver = self.running_solver()
        running_solver.forcing.clear_all()
