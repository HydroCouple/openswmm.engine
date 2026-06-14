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

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._links")

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


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------


class TestContainerProtocol:
    def test_len_matches_count(self, opened_solver):
        assert len(opened_solver.links) > 0

    def test_iter_yields_link_wrappers(self, opened_solver):
        wrappers = list(opened_solver.links)
        assert all(isinstance(w, Link) for w in wrappers)
        assert len(wrappers) == len(opened_solver.links)

    def test_getitem_int_and_str_agree(self, opened_solver):
        zero_id = opened_solver.links.get_id(0)
        by_str = opened_solver.links[zero_id]
        by_int = opened_solver.links[0]
        assert by_str == by_int
        assert by_str.id == zero_id

    def test_getitem_unknown_id_raises_keyerror(self, opened_solver):
        with pytest.raises(KeyError):
            _ = opened_solver.links["NO_SUCH_LINK_xyz"]

    def test_getitem_out_of_range_raises_indexerror(self, opened_solver):
        with pytest.raises(IndexError):
            _ = opened_solver.links[len(opened_solver.links) + 9999]

    def test_contains(self, opened_solver):
        first = opened_solver.links.get_id(0)
        assert first in opened_solver.links
        assert "NO_SUCH_LINK" not in opened_solver.links


# ---------------------------------------------------------------------------
# Link property surface
# ---------------------------------------------------------------------------


class TestLinkProperties:
    def test_identity(self, opened_solver):
        l0 = opened_solver.links[0]
        assert isinstance(l0.id, str)
        assert l0.index == 0
        assert isinstance(l0.type, LinkType)
        assert l0.solver is opened_solver

    def test_geometry_getters(self, opened_solver):
        l0 = opened_solver.links[0]
        assert isinstance(l0.length, float)
        assert isinstance(l0.roughness, float)
        assert isinstance(l0.slope, float)
        assert isinstance(l0.offset_up, float)
        assert isinstance(l0.offset_dn, float)

    def test_geometry_setter_round_trip(self, opened_solver):
        l0 = opened_solver.links[0]
        if l0.type != LinkType.CONDUIT:
            pytest.skip("length setter is only meaningful for conduits")
        original = l0.length
        l0.length = original + 12.34
        assert l0.length == pytest.approx(original + 12.34)
        l0.length = original

    def test_hydraulic_state(self, running_solver):
        l0 = running_solver.links[0]
        assert isinstance(l0.flow, float)
        assert isinstance(l0.depth, float)
        assert isinstance(l0.velocity, float)
        assert isinstance(l0.capacity, float)
        assert isinstance(l0.volume, float)

    def test_control_setting_setter(self, running_solver):
        l0 = running_solver.links[0]
        l0.control_setting = 0.5
        assert l0.control_setting == pytest.approx(0.5)

    def test_closed_setter(self, running_solver):
        l0 = running_solver.links[0]
        original = l0.closed
        l0.closed = not original
        assert l0.closed == (not original)
        l0.closed = original

    def test_loss_coeff_tuple(self, opened_solver):
        l0 = opened_solver.links[0]
        lc = l0.loss_coeff
        assert isinstance(lc, tuple) and len(lc) == 3
        l0.loss_coeff = (0.5, 0.3, 0.1)
        i, o, a = l0.loss_coeff
        assert (i, o, a) == pytest.approx((0.5, 0.3, 0.1))


# ---------------------------------------------------------------------------
# Topology — from/to are Node wrappers
# ---------------------------------------------------------------------------


class TestTopology:
    def test_from_to_are_node_wrappers(self, opened_solver):
        l0 = opened_solver.links[0]
        assert isinstance(l0.from_node, Node)
        assert isinstance(l0.to_node, Node)
        assert l0.from_node.solver is opened_solver
        assert l0.to_node.solver is opened_solver

    def test_from_to_indices_resolve(self, opened_solver):
        l0 = opened_solver.links[0]
        # Cross-check: from_node has the right id.
        assert l0.from_node.id in [n.id for n in opened_solver.nodes]
        assert l0.to_node.id in [n.id for n in opened_solver.nodes]


# ---------------------------------------------------------------------------
# Cross-section
# ---------------------------------------------------------------------------


class TestXSection:
    def test_xsect_returns_xsection(self, opened_solver):
        # Pick a conduit; orifices/weirs use the xsect slot differently.
        conduits = [l for l in opened_solver.links if l.type == LinkType.CONDUIT]
        if not conduits:
            pytest.skip("no conduits in fixture")
        x = conduits[0].xsect
        assert isinstance(x, XSection)
        assert isinstance(x.shape, XSectShape)
        assert isinstance(x.g1, float)

    def test_xsect_round_trip_tuple_setter(self, opened_solver):
        conduits = [l for l in opened_solver.links if l.type == LinkType.CONDUIT]
        if not conduits:
            pytest.skip("no conduits in fixture")
        c = conduits[0]
        original = c.xsect.as_tuple()
        c.xsect = (XSectShape.CIRCULAR, 1.5, 0.0, 0.0, 0.0)
        shape, g1, g2, g3, g4 = c.xsect.as_tuple()
        assert shape == XSectShape.CIRCULAR
        assert g1 == pytest.approx(1.5)
        # Restore.
        c.xsect = original


# ---------------------------------------------------------------------------
# Sub-views
# ---------------------------------------------------------------------------


class TestSubviews:
    def test_stats_view_always_available(self, completed_solver):
        l0 = completed_solver.links[0]
        assert isinstance(l0.stats, LinkStatsView)
        assert isinstance(l0.stats.max_flow, float)
        assert isinstance(l0.stats.max_velocity, float)
        assert isinstance(l0.stats.max_filling, float)
        assert isinstance(l0.stats.vol_flow, float)
        assert isinstance(l0.stats.surcharge_time, float)

    def test_pump_view_only_on_pump(self, opened_solver):
        non_pumps = [l for l in opened_solver.links if l.type != LinkType.PUMP]
        if not non_pumps:
            pytest.skip("fixture has only pumps")
        with pytest.raises(AttributeError):
            _ = non_pumps[0].pump

    def test_weir_view_only_on_weir(self, opened_solver):
        non_weirs = [l for l in opened_solver.links if l.type != LinkType.WEIR]
        if not non_weirs:
            pytest.skip("fixture has only weirs")
        with pytest.raises(AttributeError):
            _ = non_weirs[0].weir

    def test_orifice_view_only_on_orifice(self, opened_solver):
        non_orifices = [l for l in opened_solver.links if l.type != LinkType.ORIFICE]
        if not non_orifices:
            pytest.skip("fixture has only orifices")
        with pytest.raises(AttributeError):
            _ = non_orifices[0].orifice

    def test_outlet_view_only_on_outlet(self, opened_solver):
        non_outlets = [l for l in opened_solver.links if l.type != LinkType.OUTLET]
        if not non_outlets:
            pytest.skip("fixture has only outlets")
        with pytest.raises(AttributeError):
            _ = non_outlets[0].outlet


# ---------------------------------------------------------------------------
# Bulk numpy properties
# ---------------------------------------------------------------------------


class TestBulkProperties:
    def test_flows_round_trip(self, running_solver):
        arr = running_solver.links.flows.copy()
        running_solver.links.flows = arr
        np.testing.assert_array_equal(arr, running_solver.links.flows)

    def test_flows_setter_wrong_length_raises(self, running_solver):
        bad = np.zeros(len(running_solver.links) + 5)
        with pytest.raises(ValueError):
            running_solver.links.flows = bad

    @pytest.mark.parametrize("prop", [
        "depths", "velocities", "capacities", "volumes",
        "control_settings", "target_settings", "hyd_powers",
    ])
    def test_bulk_props_are_arrays(self, running_solver, prop):
        arr = getattr(running_solver.links, prop)
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(running_solver.links)

    def test_ids_property(self, opened_solver):
        ids = opened_solver.links.ids
        assert ids.dtype == object
        assert list(ids) == [l.id for l in opened_solver.links]

    def test_pump_stats_tuple(self, completed_solver):
        cyc, ont, vol = completed_solver.links.pump_stats()
        n = len(completed_solver.links)
        assert cyc.shape[0] == n and cyc.dtype == np.int32
        assert ont.shape[0] == n and ont.dtype == np.float64
        assert vol.shape[0] == n and vol.dtype == np.float64


# ---------------------------------------------------------------------------
# Staleness
# ---------------------------------------------------------------------------


class TestStaleness:
    def test_rename_invalidates_wrappers(self, opened_solver):
        l0 = opened_solver.links[0]
        original_id = l0.id
        new_id = original_id + "_renamed"
        opened_solver.links.rename(0, new_id)
        with pytest.raises(StaleObjectError):
            _ = l0.flow
        opened_solver.links.rename(0, original_id)


# ---------------------------------------------------------------------------
# Equality
# ---------------------------------------------------------------------------


class TestEquality:
    def test_equal_for_same_solver_index(self, opened_solver):
        a = opened_solver.links[0]
        b = opened_solver.links[0]
        assert a == b
        assert hash(a) == hash(b)

    def test_unequal_across_indices(self, opened_solver):
        if len(opened_solver.links) < 2:
            pytest.skip("need at least two links")
        assert opened_solver.links[0] != opened_solver.links[1]
