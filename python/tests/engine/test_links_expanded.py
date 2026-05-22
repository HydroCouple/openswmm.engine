"""Tests for expanded :class:`openswmm.engine.Links` bindings."""

import numpy as np
import pytest

from openswmm.engine import Links


class TestLinksConnectivity:
    """Link connectivity queries."""

    def test_get_from_node(self, links):
        n = links.get_from_node(0)
        assert isinstance(n, int)
        assert n >= 0

    def test_get_to_node(self, links):
        n = links.get_to_node(0)
        assert isinstance(n, int)
        assert n >= 0

    def test_from_to_different(self, links):
        f = links.get_from_node(0)
        t = links.get_to_node(0)
        assert f != t


class TestLinksGeometryGetters:
    """Link geometry property reads."""

    def test_get_type(self, links):
        t = links.get_type(0)
        assert isinstance(t, int)
        assert t >= 0

    def test_get_length(self, links):
        v = links.get_length(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_roughness(self, links):
        v = links.get_roughness(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_slope(self, links):
        v = links.get_slope(0)
        assert isinstance(v, float)

    def test_get_offset_up(self, links):
        v = links.get_offset_up(0)
        assert isinstance(v, float)

    def test_get_offset_dn(self, links):
        v = links.get_offset_dn(0)
        assert isinstance(v, float)

    def test_get_xsect(self, links):
        result = links.get_xsect(0)
        assert isinstance(result, tuple)
        assert len(result) == 5
        shape, g1, g2, g3, g4 = result
        assert isinstance(shape, int)

    def test_get_type_by_name(self, links):
        name = links.get_id(0)
        t = links.get_type(name)
        assert isinstance(t, int)


class TestLinksHydraulicState:
    """Expanded hydraulic state getters."""

    def test_get_velocity(self, stepped_links):
        v = stepped_links.get_velocity(0)
        assert isinstance(v, float)

    def test_get_capacity(self, stepped_links):
        v = stepped_links.get_capacity(0)
        assert isinstance(v, float)

    def test_get_volume(self, stepped_links):
        v = stepped_links.get_volume(0)
        assert isinstance(v, float)
        assert v >= 0.0


class TestLinksRuntimeForcing:
    """Runtime control methods."""

    def test_set_target_setting(self, links):
        links.set_target_setting(0, 0.5)
        v = links.get_target_setting(0)
        assert isinstance(v, float)

    def test_set_closed(self, links):
        links.set_closed(0, True)
        assert links.get_closed(0) == True
        links.set_closed(0, False)
        assert links.get_closed(0) == False


class TestLinksStatistics:
    """Link statistics after simulation."""

    def test_stat_max_flow(self, completed_solver):
        links = Links(completed_solver)
        v = links.get_stat_max_flow(0)
        assert isinstance(v, float)

    def test_stat_max_velocity(self, completed_solver):
        links = Links(completed_solver)
        v = links.get_stat_max_velocity(0)
        assert isinstance(v, float)

    def test_stat_max_filling(self, completed_solver):
        links = Links(completed_solver)
        v = links.get_stat_max_filling(0)
        assert isinstance(v, float)

    def test_stat_vol_flow(self, completed_solver):
        links = Links(completed_solver)
        v = links.get_stat_vol_flow(0)
        assert isinstance(v, float)

    def test_stat_surcharge_time(self, completed_solver):
        links = Links(completed_solver)
        v = links.get_stat_surcharge_time(0)
        assert isinstance(v, float)
        assert v >= 0.0


class TestLinksBulkExpanded:
    """Expanded bulk operations."""

    def test_get_depths_bulk(self, stepped_links):
        arr = stepped_links.get_depths_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_links.count(),)
        assert arr.dtype == np.float64


class TestLinksLossCoefficients:
    """Loss coefficient get/set."""

    def test_get_loss_coeff(self, links):
        result = links.get_loss_coeff(0)
        assert isinstance(result, tuple)
        assert len(result) == 3
        inlet, outlet, avg = result
        assert all(isinstance(v, float) for v in result)


class TestLinksFlap:
    """Flap gate get/set."""

    def test_get_flap_gate(self, links):
        v = links.get_flap_gate(0)
        assert isinstance(v, bool)

    def test_get_barrels(self, links):
        n = links.get_barrels(0)
        assert isinstance(n, int)
        assert n >= 1


# ---------------------------------------------------------------------------
# New setters (set_nodes, set_length, set_roughness)
# ---------------------------------------------------------------------------
class TestLinksSetters:
    """set_nodes / set_length / set_roughness (OPENED state)."""

    def test_set_length_roundtrip(self, opened_solver):
        orig = Links(opened_solver).get_length(0)
        new_len = orig * 1.1
        Links(opened_solver).set_length(0, new_len)
        v = Links(opened_solver).get_length(0)
        assert abs(v - new_len) < 1e-3

    def test_set_roughness_roundtrip(self, opened_solver):
        links = Links(opened_solver)
        links.set_roughness(0, 0.015)
        v = links.get_roughness(0)
        assert abs(v - 0.015) < 1e-6

    def test_set_nodes_preserves_connectivity(self, opened_solver):
        links = Links(opened_solver)
        orig_from = links.get_from_node(0)
        orig_to = links.get_to_node(0)
        # Set to same values — should succeed without error
        links.set_nodes(0, orig_from, orig_to)
        assert links.get_from_node(0) == orig_from
        assert links.get_to_node(0) == orig_to


# ---------------------------------------------------------------------------
# Rename (new addition)
# ---------------------------------------------------------------------------
class TestLinksRename:
    """rename() changes the link's string identifier."""

    def test_rename_changes_id(self, opened_solver):
        links = Links(opened_solver)
        original_name = links.get_id(0)
        new_name = original_name + "_renamed"
        links.rename(0, new_name)
        assert links.get_id(0) == new_name
        # restore
        links.rename(0, original_name)

    def test_rename_by_string_index(self, opened_solver):
        links = Links(opened_solver)
        original_name = links.get_id(0)
        new_name = original_name + "_v2"
        links.rename(original_name, new_name)
        assert links.get_index(new_name) == 0
        # restore
        links.rename(new_name, original_name)

