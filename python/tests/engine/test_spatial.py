"""Tests for :class:`openswmm.engine.Spatial` coordinate access."""

import numpy as np
import pytest


class TestSpatialNodeCoordinates:
    """Node coordinate get/set."""

    def test_set_and_get_node_coord(self, running_solver):
        from openswmm.engine import Spatial
        sp = Spatial(running_solver)
        sp.set_node_coord(0, 100.0, 200.0)
        x, y = sp.get_node_coord(0)
        assert abs(x - 100.0) < 1e-6
        assert abs(y - 200.0) < 1e-6

    def test_get_node_coords_bulk(self, running_solver):
        from openswmm.engine import Spatial, Nodes
        sp = Spatial(running_solver)
        nodes = Nodes(running_solver)
        n = nodes.count()
        x_arr, y_arr = sp.get_node_coords_bulk()
        assert isinstance(x_arr, np.ndarray)
        assert isinstance(y_arr, np.ndarray)
        assert x_arr.shape == (n,)
        assert y_arr.shape == (n,)


class TestSpatialLinkCoordinates:
    """Link coordinate get/set."""

    def test_set_and_get_link_coord(self, running_solver):
        from openswmm.engine import Spatial
        sp = Spatial(running_solver)
        sp.set_link_coord(0, 50.0, 75.0)
        x, y = sp.get_link_coord(0)
        assert abs(x - 50.0) < 1e-6
        assert abs(y - 75.0) < 1e-6


class TestSpatialSubcatchCoordinates:
    """Subcatchment coordinate get/set."""

    def test_set_and_get_subcatch_coord(self, running_solver):
        from openswmm.engine import Spatial
        sp = Spatial(running_solver)
        sp.set_subcatch_coord(0, 300.0, 400.0)
        x, y = sp.get_subcatch_coord(0)
        assert abs(x - 300.0) < 1e-6
        assert abs(y - 400.0) < 1e-6


class TestSpatialGageCoordinates:
    """Gage coordinate get/set."""

    def test_set_and_get_gage_coord(self, running_solver):
        from openswmm.engine import Spatial
        sp = Spatial(running_solver)
        sp.set_gage_coord(0, 10.0, 20.0)
        x, y = sp.get_gage_coord(0)
        assert abs(x - 10.0) < 1e-6
        assert abs(y - 20.0) < 1e-6


class TestSpatialCRS:
    """CRS get/set."""

    def test_set_and_get_crs(self, running_solver):
        from openswmm.engine import Spatial
        sp = Spatial(running_solver)
        sp.set_crs("EPSG:4326")
        crs = sp.get_crs()
        assert crs == "EPSG:4326"
