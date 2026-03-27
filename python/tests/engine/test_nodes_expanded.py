"""Tests for expanded :class:`openswmm.engine.Nodes` bindings."""

import numpy as np
import pytest

from openswmm.engine import Nodes


class TestNodesGeometryGetters:
    """Node geometry property reads."""

    def test_get_type(self, nodes):
        t = nodes.get_type(0)
        assert isinstance(t, int)
        assert t >= 0

    def test_get_invert_elev(self, nodes):
        v = nodes.get_invert_elev(0)
        assert isinstance(v, float)

    def test_get_max_depth(self, nodes):
        v = nodes.get_max_depth(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_surcharge_depth(self, nodes):
        v = nodes.get_surcharge_depth(0)
        assert isinstance(v, float)

    def test_get_ponded_area(self, nodes):
        v = nodes.get_ponded_area(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_initial_depth(self, nodes):
        v = nodes.get_initial_depth(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_crown_elev(self, nodes):
        v = nodes.get_crown_elev(0)
        assert isinstance(v, float)

    def test_get_full_volume(self, nodes):
        v = nodes.get_full_volume(0)
        assert isinstance(v, float)

    def test_get_degree(self, nodes):
        d = nodes.get_degree(0)
        assert isinstance(d, int)
        assert d >= 0

    def test_get_type_by_name(self, nodes):
        name = nodes.get_id(0)
        t = nodes.get_type(name)
        assert isinstance(t, int)


class TestNodesHydraulicState:
    """Expanded hydraulic state getters."""

    def test_get_lateral_inflow(self, stepped_nodes):
        v = stepped_nodes.get_lateral_inflow(0)
        assert isinstance(v, float)

    def test_get_overflow(self, stepped_nodes):
        v = stepped_nodes.get_overflow(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_inflow(self, stepped_nodes):
        v = stepped_nodes.get_inflow(0)
        assert isinstance(v, float)

    def test_get_losses(self, stepped_nodes):
        v = stepped_nodes.get_losses(0)
        assert isinstance(v, float)

    def test_get_outflow(self, stepped_nodes):
        v = stepped_nodes.get_outflow(0)
        assert isinstance(v, float)


class TestNodesGeometrySetters:
    """Node geometry property writes (requires OPENED state, not STARTED)."""

    def test_set_invert_elev(self, opened_solver):
        n = Nodes(opened_solver)
        n.set_invert_elev(0, 100.5)
        v = n.get_invert_elev(0)
        assert abs(v - 100.5) < 1e-6

    def test_set_pond_area(self, opened_solver):
        n = Nodes(opened_solver)
        n.set_pond_area(0, 500.0)
        v = n.get_ponded_area(0)
        assert abs(v - 500.0) < 1e-6


class TestNodesStatistics:
    """Node statistics after simulation."""

    def test_stat_max_depth(self, completed_solver):
        nodes = Nodes(completed_solver)
        v = nodes.get_stat_max_depth(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_stat_max_overflow(self, completed_solver):
        nodes = Nodes(completed_solver)
        v = nodes.get_stat_max_overflow(0)
        assert isinstance(v, float)

    def test_stat_vol_flooded(self, completed_solver):
        nodes = Nodes(completed_solver)
        v = nodes.get_stat_vol_flooded(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_stat_time_flooded(self, completed_solver):
        nodes = Nodes(completed_solver)
        v = nodes.get_stat_time_flooded(0)
        assert isinstance(v, float)
        assert v >= 0.0


class TestNodesBulkExpanded:
    """Expanded bulk operations."""

    def test_get_inflows_bulk(self, stepped_nodes):
        arr = stepped_nodes.get_inflows_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_nodes.count(),)

    def test_get_overflows_bulk(self, stepped_nodes):
        arr = stepped_nodes.get_overflows_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_nodes.count(),)

    def test_set_lat_inflows_bulk(self, nodes):
        n = nodes.count()
        inflows = np.full(n, 0.1, dtype=np.float64)
        nodes.set_lat_inflows_bulk(inflows)
