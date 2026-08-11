"""
P3 — Links collection + Link wrapper Pythonic surface tests.

Mirrors :mod:`test_nodes_pythonic` and additionally checks:

* Topology — ``link.from_node`` / ``link.to_node`` yield :class:`Node`
  wrappers and round-trip via index;
* Cross-section property: getter returns an :class:`XSection`,
  ``link.xsect = (shape, g1..g4)`` writes through;
* Per-type sub-views (``pump`` / ``weir`` / ``orifice`` / ``outlet``)
  with the same wrong-type ``AttributeError`` contract as nodes.
"""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._links  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (  # noqa: E402
    LinkType,
    NodeType,
    OrificeType,
    OutletRatingType,
    StaleObjectError,
    WeirType,
    XSectShape,
)
from openswmm.engine._links import (  # noqa: E402
    Link,
    LinkStatsView,
    Links,
    OrificeView,
    OutletView,
    PumpView,
    WeirView,
    XSection,
)
from openswmm.engine._nodes import Node  # noqa: E402

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------


class TestContainerProtocol(EngineSolverCase):
    def test_len_matches_count(self):
        solver = self.opened_solver()
        self.assertGreater(len(solver.links), 0)

    def test_iter_yields_link_wrappers(self):
        solver = self.opened_solver()
        wrappers = list(solver.links)
        self.assertTrue(all(isinstance(w, Link) for w in wrappers))
        self.assertEqual(len(wrappers), len(solver.links))

    def test_getitem_int_and_str_agree(self):
        solver = self.opened_solver()
        zero_id = solver.links.get_id(0)
        by_str = solver.links[zero_id]
        by_int = solver.links[0]
        self.assertEqual(by_str, by_int)
        self.assertEqual(by_str.id, zero_id)

    def test_getitem_unknown_id_raises_keyerror(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            _ = solver.links["NO_SUCH_LINK_xyz"]

    def test_getitem_out_of_range_raises_indexerror(self):
        solver = self.opened_solver()
        with self.assertRaises(IndexError):
            _ = solver.links[len(solver.links) + 9999]

    def test_contains(self):
        solver = self.opened_solver()
        first = solver.links.get_id(0)
        self.assertIn(first, solver.links)
        self.assertNotIn("NO_SUCH_LINK", solver.links)


# ---------------------------------------------------------------------------
# Link property surface
# ---------------------------------------------------------------------------


class TestLinkProperties(EngineSolverCase):
    def test_identity(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        self.assertIsInstance(l0.id, str)
        self.assertEqual(l0.index, 0)
        self.assertIsInstance(l0.type, LinkType)
        self.assertIs(l0.solver, solver)

    def test_geometry_getters(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        self.assertIsInstance(l0.length, float)
        self.assertIsInstance(l0.roughness, float)
        self.assertIsInstance(l0.slope, float)
        self.assertIsInstance(l0.offset_up, float)
        self.assertIsInstance(l0.offset_dn, float)

    def test_geometry_setter_round_trip(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        if l0.type != LinkType.CONDUIT:
            self.skipTest("length setter is only meaningful for conduits")
        original = l0.length
        l0.length = original + 12.34
        self.assertAlmostEqual(l0.length, original + 12.34, places=6)
        l0.length = original

    def test_hydraulic_state(self):
        solver = self.running_solver()
        l0 = solver.links[0]
        self.assertIsInstance(l0.flow, float)
        self.assertIsInstance(l0.depth, float)
        self.assertIsInstance(l0.velocity, float)
        self.assertIsInstance(l0.capacity, float)
        self.assertIsInstance(l0.volume, float)

    def test_control_setting_setter(self):
        solver = self.running_solver()
        l0 = solver.links[0]
        l0.control_setting = 0.5
        self.assertAlmostEqual(l0.control_setting, 0.5, places=6)

    def test_closed_setter(self):
        solver = self.running_solver()
        l0 = solver.links[0]
        original = l0.closed
        l0.closed = not original
        self.assertEqual(l0.closed, (not original))
        l0.closed = original

    def test_loss_coeff_tuple(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        lc = l0.loss_coeff
        self.assertIsInstance(lc, tuple)
        self.assertEqual(len(lc), 3)
        l0.loss_coeff = (0.5, 0.3, 0.1)
        i, o, a = l0.loss_coeff
        self.assertAlmostEqual(i, 0.5, places=6)
        self.assertAlmostEqual(o, 0.3, places=6)
        self.assertAlmostEqual(a, 0.1, places=6)


# ---------------------------------------------------------------------------
# Topology — from/to are Node wrappers
# ---------------------------------------------------------------------------


class TestTopology(EngineSolverCase):
    def test_from_to_are_node_wrappers(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        self.assertIsInstance(l0.from_node, Node)
        self.assertIsInstance(l0.to_node, Node)
        self.assertIs(l0.from_node.solver, solver)
        self.assertIs(l0.to_node.solver, solver)

    def test_from_to_indices_resolve(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        # Cross-check: from_node has the right id.
        self.assertIn(l0.from_node.id, [n.id for n in solver.nodes])
        self.assertIn(l0.to_node.id, [n.id for n in solver.nodes])


# ---------------------------------------------------------------------------
# Cross-section
# ---------------------------------------------------------------------------


class TestXSection(EngineSolverCase):
    def test_xsect_returns_xsection(self):
        solver = self.opened_solver()
        # Pick a conduit; orifices/weirs use the xsect slot differently.
        conduits = [l for l in solver.links if l.type == LinkType.CONDUIT]
        if not conduits:
            self.skipTest("no conduits in fixture")
        x = conduits[0].xsect
        self.assertIsInstance(x, XSection)
        self.assertIsInstance(x.shape, XSectShape)
        self.assertIsInstance(x.g1, float)

    def test_xsect_round_trip_tuple_setter(self):
        solver = self.opened_solver()
        conduits = [l for l in solver.links if l.type == LinkType.CONDUIT]
        if not conduits:
            self.skipTest("no conduits in fixture")
        c = conduits[0]
        original = c.xsect.as_tuple()
        c.xsect = (XSectShape.CIRCULAR, 1.5, 0.0, 0.0, 0.0)
        shape, g1, g2, g3, g4 = c.xsect.as_tuple()
        self.assertEqual(shape, XSectShape.CIRCULAR)
        self.assertAlmostEqual(g1, 1.5, places=6)
        # Restore.
        c.xsect = original


# ---------------------------------------------------------------------------
# Sub-views
# ---------------------------------------------------------------------------


class TestSubviews(EngineSolverCase):
    def test_stats_view_always_available(self):
        solver = self.completed_solver()
        l0 = solver.links[0]
        self.assertIsInstance(l0.stats, LinkStatsView)
        self.assertIsInstance(l0.stats.max_flow, float)
        self.assertIsInstance(l0.stats.max_velocity, float)
        self.assertIsInstance(l0.stats.max_filling, float)
        self.assertIsInstance(l0.stats.vol_flow, float)
        self.assertIsInstance(l0.stats.surcharge_time, float)

    def test_pump_view_only_on_pump(self):
        solver = self.opened_solver()
        non_pumps = [l for l in solver.links if l.type != LinkType.PUMP]
        if not non_pumps:
            self.skipTest("fixture has only pumps")
        with self.assertRaises(AttributeError):
            _ = non_pumps[0].pump

    def test_weir_view_only_on_weir(self):
        solver = self.opened_solver()
        non_weirs = [l for l in solver.links if l.type != LinkType.WEIR]
        if not non_weirs:
            self.skipTest("fixture has only weirs")
        with self.assertRaises(AttributeError):
            _ = non_weirs[0].weir

    def test_orifice_view_only_on_orifice(self):
        solver = self.opened_solver()
        non_orifices = [l for l in solver.links if l.type != LinkType.ORIFICE]
        if not non_orifices:
            self.skipTest("fixture has only orifices")
        with self.assertRaises(AttributeError):
            _ = non_orifices[0].orifice

    def test_outlet_view_only_on_outlet(self):
        solver = self.opened_solver()
        non_outlets = [l for l in solver.links if l.type != LinkType.OUTLET]
        if not non_outlets:
            self.skipTest("fixture has only outlets")
        with self.assertRaises(AttributeError):
            _ = non_outlets[0].outlet


# ---------------------------------------------------------------------------
# Bulk numpy properties
# ---------------------------------------------------------------------------


class TestBulkProperties(EngineSolverCase):
    def test_flows_round_trip(self):
        solver = self.running_solver()
        arr = solver.links.flows.copy()
        solver.links.flows = arr
        np.testing.assert_array_equal(arr, solver.links.flows)

    def test_flows_setter_wrong_length_raises(self):
        solver = self.running_solver()
        bad = np.zeros(len(solver.links) + 5)
        with self.assertRaises(ValueError):
            solver.links.flows = bad

    def test_bulk_props_are_arrays(self):
        solver = self.running_solver()
        for prop in (
            "depths", "velocities", "capacities", "volumes",
            "control_settings", "target_settings", "hyd_powers",
        ):
            with self.subTest(prop=prop):
                arr = getattr(solver.links, prop)
                self.assertIsInstance(arr, np.ndarray)
                self.assertEqual(arr.dtype, np.float64)
                self.assertEqual(arr.shape[0], len(solver.links))

    def test_ids_property(self):
        solver = self.opened_solver()
        ids = solver.links.ids
        self.assertEqual(ids.dtype, object)
        self.assertEqual(list(ids), [l.id for l in solver.links])

    def test_pump_stats_tuple(self):
        solver = self.completed_solver()
        cyc, ont, vol = solver.links.pump_stats()
        n = len(solver.links)
        self.assertEqual(cyc.shape[0], n)
        self.assertEqual(cyc.dtype, np.int32)
        self.assertEqual(ont.shape[0], n)
        self.assertEqual(ont.dtype, np.float64)
        self.assertEqual(vol.shape[0], n)
        self.assertEqual(vol.dtype, np.float64)


# ---------------------------------------------------------------------------
# Staleness
# ---------------------------------------------------------------------------


class TestStaleness(EngineSolverCase):
    def test_rename_invalidates_wrappers(self):
        solver = self.opened_solver()
        l0 = solver.links[0]
        original_id = l0.id
        new_id = original_id + "_renamed"
        solver.links.rename(0, new_id)
        with self.assertRaises(StaleObjectError):
            _ = l0.flow
        solver.links.rename(0, original_id)


# ---------------------------------------------------------------------------
# Equality
# ---------------------------------------------------------------------------


class TestEquality(EngineSolverCase):
    def test_equal_for_same_solver_index(self):
        solver = self.opened_solver()
        a = solver.links[0]
        b = solver.links[0]
        self.assertEqual(a, b)
        self.assertEqual(hash(a), hash(b))

    def test_unequal_across_indices(self):
        solver = self.opened_solver()
        if len(solver.links) < 2:
            self.skipTest("need at least two links")
        self.assertNotEqual(solver.links[0], solver.links[1])
