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

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._nodes")

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


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------


class TestContainerProtocol:
    def test_len_matches_count(self, opened_solver):
        n = len(opened_solver.nodes)
        assert n > 0

    def test_iter_yields_node_wrappers(self, opened_solver):
        wrappers = list(opened_solver.nodes)
        assert all(isinstance(w, Node) for w in wrappers)
        assert len(wrappers) == len(opened_solver.nodes)

    def test_getitem_int_and_str_agree(self, opened_solver):
        zero_id = opened_solver.nodes.get_id(0)
        by_str = opened_solver.nodes[zero_id]
        by_int = opened_solver.nodes[0]
        assert by_str == by_int
        assert by_str.id == zero_id

    def test_getitem_unknown_id_raises_keyerror(self, opened_solver):
        with pytest.raises(KeyError):
            _ = opened_solver.nodes["NO_SUCH_NODE_xyz"]

    def test_getitem_out_of_range_raises_indexerror(self, opened_solver):
        n = len(opened_solver.nodes)
        with pytest.raises(IndexError):
            _ = opened_solver.nodes[n + 9999]

    def test_getitem_wrong_type_raises_typeerror(self, opened_solver):
        with pytest.raises(TypeError):
            _ = opened_solver.nodes[3.14]

    def test_contains(self, opened_solver):
        first = opened_solver.nodes.get_id(0)
        assert first in opened_solver.nodes
        assert "NO_SUCH_NODE" not in opened_solver.nodes


# ---------------------------------------------------------------------------
# Wrapper property surface
# ---------------------------------------------------------------------------


class TestNodeProperties:
    def test_identity(self, opened_solver):
        n0 = opened_solver.nodes[0]
        assert isinstance(n0.id, str)
        assert n0.index == 0
        assert isinstance(n0.type, NodeType)
        assert n0.solver is opened_solver

    def test_geometry_getters(self, opened_solver):
        n0 = opened_solver.nodes[0]
        # All geometry getters return a number; specific values depend on the
        # fixture model so we just smoke-check the types.
        assert isinstance(n0.invert_elev, float)
        assert isinstance(n0.max_depth, float)
        assert isinstance(n0.surcharge_depth, float)
        assert isinstance(n0.ponded_area, float)
        assert isinstance(n0.initial_depth, float)
        assert isinstance(n0.crown_elev, float)
        assert isinstance(n0.full_volume, float)
        assert isinstance(n0.degree, int)

    def test_geometry_setter_round_trip(self, opened_solver):
        n0 = opened_solver.nodes[0]
        original = n0.invert_elev
        n0.invert_elev = original + 1.5
        assert n0.invert_elev == pytest.approx(original + 1.5)
        n0.invert_elev = original  # restore

    def test_hydraulic_state(self, running_solver):
        # Pick first node; runtime state values are only meaningful in RUNNING.
        n0 = running_solver.nodes[0]
        assert isinstance(n0.depth, float)
        assert isinstance(n0.head, float)
        assert isinstance(n0.volume, float)
        assert isinstance(n0.lateral_inflow, float)
        assert isinstance(n0.overflow, float)
        assert isinstance(n0.inflow, float)

    def test_lateral_inflow_setter(self, running_solver):
        n0 = running_solver.nodes[0]
        n0.lateral_inflow = 0.42
        assert n0.lateral_inflow == pytest.approx(0.42)


# ---------------------------------------------------------------------------
# Sub-views
# ---------------------------------------------------------------------------


class TestSubviews:
    def test_stats_view(self, completed_solver):
        n0 = completed_solver.nodes[0]
        assert isinstance(n0.stats, NodeStatsView)
        # All four floats; values vary by model.
        assert isinstance(n0.stats.max_depth, float)
        assert isinstance(n0.stats.max_overflow, float)
        assert isinstance(n0.stats.vol_flooded, float)
        assert isinstance(n0.stats.time_flooded, float)

    def test_outfall_view_only_on_outfall_nodes(self, opened_solver):
        nodes = opened_solver.nodes
        # The fixture has at least one outfall — find it.
        outfalls = [n for n in nodes if n.type == NodeType.OUTFALL]
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not outfalls or not junctions:
            pytest.skip("fixture lacks both outfalls and junctions")
        ofv = outfalls[0].outfall
        assert isinstance(ofv, OutfallView)
        assert isinstance(ofv.type, OutfallType)
        # Junction must reject .outfall access.
        with pytest.raises(AttributeError):
            _ = junctions[0].outfall

    def test_storage_view_only_on_storage_nodes(self, opened_solver):
        nodes = opened_solver.nodes
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not junctions:
            pytest.skip("fixture has no junctions")
        with pytest.raises(AttributeError):
            _ = junctions[0].storage

    def test_divider_view_only_on_divider_nodes(self, opened_solver):
        nodes = opened_solver.nodes
        junctions = [n for n in nodes if n.type == NodeType.JUNCTION]
        if not junctions:
            pytest.skip("fixture has no junctions")
        with pytest.raises(AttributeError):
            _ = junctions[0].divider


# ---------------------------------------------------------------------------
# Bulk numpy properties
# ---------------------------------------------------------------------------


class TestBulkProperties:
    def test_depths_property(self, stepped_solver):
        arr = stepped_solver.nodes.depths
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(stepped_solver.nodes)

    def test_depths_setter(self, stepped_solver):
        arr = stepped_solver.nodes.depths.copy()
        stepped_solver.nodes.depths = arr  # round-trip should be a no-op
        np.testing.assert_array_equal(arr, stepped_solver.nodes.depths)

    def test_depths_setter_wrong_length_raises(self, stepped_solver):
        bad = np.zeros(len(stepped_solver.nodes) + 7)
        with pytest.raises(ValueError):
            stepped_solver.nodes.depths = bad

    @pytest.mark.parametrize("prop", [
        "heads", "inflows", "overflows", "volumes",
        "outflows", "losses", "lateral_inflows",
    ])
    def test_other_bulk_props_are_arrays(self, stepped_solver, prop):
        arr = getattr(stepped_solver.nodes, prop)
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(stepped_solver.nodes)

    def test_ids_is_object_array(self, opened_solver):
        ids = opened_solver.nodes.ids
        assert isinstance(ids, np.ndarray)
        assert ids.dtype == object
        assert len(ids) == len(opened_solver.nodes)
        # Same as iterating .id on each wrapper.
        wrapper_ids = [n.id for n in opened_solver.nodes]
        assert list(ids) == wrapper_ids


# ---------------------------------------------------------------------------
# Editing + staleness contract
# ---------------------------------------------------------------------------


class TestStaleness:
    def test_rename_invalidates_wrappers(self, opened_solver):
        n0 = opened_solver.nodes[0]
        original_id = n0.id
        new_id = original_id + "_renamed"
        opened_solver.nodes.rename(0, new_id)
        # Old wrapper is stale; access raises StaleObjectError.
        with pytest.raises(StaleObjectError):
            _ = n0.depth
        # Re-look-up by the new id works.
        n0b = opened_solver.nodes[new_id]
        assert n0b.id == new_id
        # Restore for fixture re-use safety.
        opened_solver.nodes.rename(0, original_id)


# ---------------------------------------------------------------------------
# Equality / hashing
# ---------------------------------------------------------------------------


class TestEquality:
    def test_equal_for_same_solver_index(self, opened_solver):
        a = opened_solver.nodes[0]
        b = opened_solver.nodes[0]
        assert a == b
        assert hash(a) == hash(b)

    def test_unequal_across_indices(self, opened_solver):
        if len(opened_solver.nodes) < 2:
            pytest.skip("need at least two nodes")
        assert opened_solver.nodes[0] != opened_solver.nodes[1]

    def test_unequal_to_non_node(self, opened_solver):
        assert opened_solver.nodes[0] != "J1"
        assert opened_solver.nodes[0] != 0
