"""
P2 — Nodes collection + Node wrapper Pythonic surface tests.

Exercises every entry point on :class:`openswmm.engine.Nodes` and the
returned :class:`openswmm.engine._nodes.Node` wrappers:

* Container protocol (``__len__``, ``__iter__``, ``__getitem__``,
  ``__contains__``) with both ``int`` and ``str`` keys.
* Property access on the wrapper (geometry, hydraulic state, identity).
* Property setters (and the staleness contract after a rename).
* Bulk numpy accessors as properties.
* Per-type sub-views (``storage`` / ``outfall`` / ``divider``) — including
  the ``AttributeError`` raised on wrong-type access.
* Statistics sub-view.
* Equality / hashing of wrappers.

Requires the compiled engine; the entire module is skipped at collection
when ``openswmm.engine._nodes`` cannot be imported.
"""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._nodes  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (  # noqa: E402
    NodeType,
    OutfallType,
    StaleObjectError,
)
from openswmm.engine._nodes import (  # noqa: E402
    DividerView,
    Node,
    NodeStatsView,
    Nodes,
    OutfallView,
    StorageView,
)

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------


class TestContainerProtocol(EngineSolverCase):
    def test_len_matches_count(self):
        solver = self.opened_solver()
        n = len(solver.nodes)
        self.assertGreater(n, 0)

    def test_iter_yields_node_wrappers(self):
        solver = self.opened_solver()
        wrappers = list(solver.nodes)
        self.assertTrue(all(isinstance(w, Node) for w in wrappers))
        self.assertEqual(len(wrappers), len(solver.nodes))

    def test_getitem_int_and_str_agree(self):
        solver = self.opened_solver()
        zero_id = solver.nodes.get_id(0)
        by_str = solver.nodes[zero_id]
        by_int = solver.nodes[0]
        self.assertEqual(by_str, by_int)
        self.assertEqual(by_str.id, zero_id)

    def test_getitem_unknown_id_raises_keyerror(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            _ = solver.nodes["NO_SUCH_NODE_xyz"]

    def test_getitem_out_of_range_raises_indexerror(self):
        solver = self.opened_solver()
        n = len(solver.nodes)
        with self.assertRaises(IndexError):
            _ = solver.nodes[n + 9999]

    def test_getitem_wrong_type_raises_typeerror(self):
        solver = self.opened_solver()
        with self.assertRaises(TypeError):
            _ = solver.nodes[3.14]

    def test_contains(self):
        solver = self.opened_solver()
        first = solver.nodes.get_id(0)
        self.assertIn(first, solver.nodes)
        self.assertNotIn("NO_SUCH_NODE", solver.nodes)


# ---------------------------------------------------------------------------
# Wrapper property surface
# ---------------------------------------------------------------------------


class TestNodeProperties(EngineSolverCase):
    def test_identity(self):
        solver = self.opened_solver()
        n0 = solver.nodes[0]
        self.assertIsInstance(n0.id, str)
        self.assertEqual(n0.index, 0)
        self.assertIsInstance(n0.type, NodeType)
        self.assertIs(n0.solver, solver)

    def test_geometry_getters(self):
        solver = self.opened_solver()
        n0 = solver.nodes[0]
        # All geometry getters return a number; specific values depend on the
        # fixture model so we just smoke-check the types.
        self.assertIsInstance(n0.invert_elev, float)
        self.assertIsInstance(n0.max_depth, float)
        self.assertIsInstance(n0.surcharge_depth, float)
        self.assertIsInstance(n0.ponded_area, float)
        self.assertIsInstance(n0.initial_depth, float)
        self.assertIsInstance(n0.crown_elev, float)
        self.assertIsInstance(n0.full_volume, float)
        self.assertIsInstance(n0.degree, int)

    def test_geometry_setter_round_trip(self):
        solver = self.opened_solver()
        n0 = solver.nodes[0]
        original = n0.invert_elev
        n0.invert_elev = original + 1.5
        self.assertAlmostEqual(n0.invert_elev, original + 1.5, places=7)
        n0.invert_elev = original  # restore

    def test_hydraulic_state(self):
        # Pick first node; runtime state values are only meaningful in RUNNING.
        solver = self.running_solver()
        n0 = solver.nodes[0]
        self.assertIsInstance(n0.depth, float)
        self.assertIsInstance(n0.head, float)
        self.assertIsInstance(n0.volume, float)
        self.assertIsInstance(n0.lateral_inflow, float)
        self.assertIsInstance(n0.overflow, float)
        self.assertIsInstance(n0.inflow, float)

    def test_outflow_property(self):
        """Node.outflow returns a float >= 0 after stepping."""
        solver = self.stepped_solver()
        n0 = solver.nodes[0]
        self.assertIsInstance(n0.outflow, float)
        self.assertGreaterEqual(n0.outflow, 0.0)

    def test_lateral_inflow_setter(self):
        # ``lateral_inflow`` is a one-step forcing override: the setter writes a
        # user forcing buffer (``user_lat_flow``) that the engine folds into the
        # realized lateral inflow during the next routing step. The getter reads
        # the realized state (``lat_flow``), so the override only becomes visible
        # after ``step()`` — there is no same-tick round-trip on this property.
        solver = self.running_solver()
        n0 = solver.nodes[0]
        n0.lateral_inflow = 0.42
        solver.step()
        self.assertAlmostEqual(solver.nodes[0].lateral_inflow, 0.42, places=6)


# ---------------------------------------------------------------------------
# Sub-views
# ---------------------------------------------------------------------------


class TestSubviews(EngineSolverCase):
    def test_stats_view(self):
        solver = self.completed_solver()
        n0 = solver.nodes[0]
        self.assertIsInstance(n0.stats, NodeStatsView)
        # All four floats; values vary by model.
        self.assertIsInstance(n0.stats.max_depth, float)
        self.assertIsInstance(n0.stats.max_overflow, float)
        self.assertIsInstance(n0.stats.vol_flooded, float)
        self.assertIsInstance(n0.stats.time_flooded, float)

    def test_outfall_view_only_on_outfall_nodes(self):
        solver = self.opened_solver()
        nodes = solver.nodes
        # The fixture has at least one outfall — find it.
        outfalls = [n for n in nodes if n.type == NodeType.OUTFALL]
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not outfalls or not junctions:
            self.skipTest("fixture lacks both outfalls and junctions")
        ofv = outfalls[0].outfall
        self.assertIsInstance(ofv, OutfallView)
        self.assertIsInstance(ofv.type, OutfallType)
        # Junction must reject .outfall access.
        with self.assertRaises(AttributeError):
            _ = junctions[0].outfall

    def test_storage_view_only_on_storage_nodes(self):
        solver = self.opened_solver()
        nodes = solver.nodes
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not junctions:
            self.skipTest("fixture has no junctions")
        with self.assertRaises(AttributeError):
            _ = junctions[0].storage

    def test_divider_view_only_on_divider_nodes(self):
        solver = self.opened_solver()
        nodes = solver.nodes
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not junctions:
            self.skipTest("fixture has no junctions")
        with self.assertRaises(AttributeError):
            _ = junctions[0].divider


# ---------------------------------------------------------------------------
# Bulk numpy properties
# ---------------------------------------------------------------------------


class TestBulkProperties(EngineSolverCase):
    def test_depths_property(self):
        solver = self.stepped_solver()
        arr = solver.nodes.depths
        self.assertIsInstance(arr, np.ndarray)
        self.assertEqual(arr.dtype, np.float64)
        self.assertEqual(arr.shape[0], len(solver.nodes))

    def test_depths_setter(self):
        solver = self.stepped_solver()
        arr = solver.nodes.depths.copy()
        solver.nodes.depths = arr  # round-trip should be a no-op
        np.testing.assert_array_equal(arr, solver.nodes.depths)

    def test_depths_setter_wrong_length_raises(self):
        solver = self.stepped_solver()
        bad = np.zeros(len(solver.nodes) + 7)
        with self.assertRaises(ValueError):
            solver.nodes.depths = bad

    def test_other_bulk_props_are_arrays(self):
        solver = self.stepped_solver()
        for prop in (
            "heads", "inflows", "overflows", "volumes",
            "outflows", "losses", "lateral_inflows",
        ):
            with self.subTest(prop=prop):
                arr = getattr(solver.nodes, prop)
                self.assertIsInstance(arr, np.ndarray)
                self.assertEqual(arr.dtype, np.float64)
                self.assertEqual(arr.shape[0], len(solver.nodes))

    def test_ids_is_object_array(self):
        solver = self.opened_solver()
        ids = solver.nodes.ids
        self.assertIsInstance(ids, np.ndarray)
        self.assertEqual(ids.dtype, object)
        self.assertEqual(len(ids), len(solver.nodes))
        # Same as iterating .id on each wrapper.
        wrapper_ids = [n.id for n in solver.nodes]
        self.assertEqual(list(ids), wrapper_ids)


# ---------------------------------------------------------------------------
# Editing + staleness contract
# ---------------------------------------------------------------------------


class TestStaleness(EngineSolverCase):
    def test_rename_invalidates_wrappers(self):
        solver = self.opened_solver()
        n0 = solver.nodes[0]
        original_id = n0.id
        new_id = original_id + "_renamed"
        solver.nodes.rename(0, new_id)
        # Old wrapper is stale; access raises StaleObjectError.
        with self.assertRaises(StaleObjectError):
            _ = n0.depth
        # Re-look-up by the new id works.
        n0b = solver.nodes[new_id]
        self.assertEqual(n0b.id, new_id)
        # Restore for fixture re-use safety.
        solver.nodes.rename(0, original_id)


# ---------------------------------------------------------------------------
# Equality / hashing
# ---------------------------------------------------------------------------


class TestEquality(EngineSolverCase):
    def test_equal_for_same_solver_index(self):
        solver = self.opened_solver()
        a = solver.nodes[0]
        b = solver.nodes[0]
        self.assertEqual(a, b)
        self.assertEqual(hash(a), hash(b))

    def test_unequal_across_indices(self):
        solver = self.opened_solver()
        if len(solver.nodes) < 2:
            self.skipTest("need at least two nodes")
        self.assertNotEqual(solver.nodes[0], solver.nodes[1])

    def test_unequal_to_non_node(self):
        solver = self.opened_solver()
        self.assertNotEqual(solver.nodes[0], "J1")
        self.assertNotEqual(solver.nodes[0], 0)
