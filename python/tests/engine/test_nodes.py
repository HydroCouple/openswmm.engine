"""Tests for :class:`openswmm.engine.Nodes` node access during simulation."""

import numpy as np
import pytest

from openswmm.engine import Nodes


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestNodesConstruction:
    """Nodes instantiation from a Solver."""

    def test_create_from_solver(self, running_solver):
        nodes = Nodes(running_solver)
        assert nodes is not None

    def test_count(self, nodes):
        n = nodes.count()
        assert isinstance(n, int)
        assert n == 12  # site-drainage example has 12 nodes


# ---------------------------------------------------------------------------
# Index / ID lookup
# ---------------------------------------------------------------------------
class TestNodesLookup:
    """Name ↔ index resolution."""

    def test_get_index_valid(self, nodes):
        idx = nodes.get_index("J1")
        assert isinstance(idx, int)
        assert idx >= 0

    def test_get_index_not_found(self, nodes):
        idx = nodes.get_index("NONEXISTENT")
        assert idx == -1

    def test_get_id(self, nodes):
        name = nodes.get_id(0)
        assert isinstance(name, str)
        assert len(name) > 0

    def test_roundtrip_id_index(self, nodes):
        """get_id(get_index(name)) == name."""
        name = "J1"
        idx = nodes.get_index(name)
        assert nodes.get_id(idx) == name


# ---------------------------------------------------------------------------
# Getters
# ---------------------------------------------------------------------------
class TestNodesGetters:
    """Individual node property reads."""

    def test_get_depth_initial(self, nodes):
        d = nodes.get_depth(0)
        assert isinstance(d, float)
        assert d >= 0.0

    def test_get_depth_after_steps(self, stepped_nodes):
        d = stepped_nodes.get_depth(0)
        assert isinstance(d, float)

    def test_get_head(self, stepped_nodes):
        h = stepped_nodes.get_head(0)
        assert isinstance(h, float)

    def test_get_volume(self, stepped_nodes):
        v = stepped_nodes.get_volume(0)
        assert isinstance(v, float)
        assert v >= 0.0


# ---------------------------------------------------------------------------
# Setters
# ---------------------------------------------------------------------------
class TestNodesSetters:
    """Individual node property writes."""

    def test_set_depth(self, nodes):
        nodes.set_depth(0, 1.5)
        d = nodes.get_depth(0)
        assert abs(d - 1.5) < 1e-6

    def test_set_lateral_inflow(self, nodes):
        # Should not raise in RUNNING state
        nodes.set_lateral_inflow(0, 0.5)


# ---------------------------------------------------------------------------
# Bulk operations
# ---------------------------------------------------------------------------
class TestNodesBulk:
    """Bulk array get/set using NumPy."""

    def test_get_depths_bulk_shape(self, nodes):
        arr = nodes.get_depths_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape == (nodes.count(),)

    def test_get_heads_bulk_shape(self, nodes):
        arr = nodes.get_heads_bulk()
        assert arr.shape == (nodes.count(),)
        assert arr.dtype == np.float64

    def test_set_depths_bulk(self, nodes):
        n = nodes.count()
        new_depths = np.full(n, 2.0, dtype=np.float64)
        nodes.set_depths_bulk(new_depths)
        result = nodes.get_depths_bulk()
        np.testing.assert_allclose(result, new_depths, atol=1e-10)

    def test_get_depths_bulk_after_steps(self, stepped_nodes):
        arr = stepped_nodes.get_depths_bulk()
        # After simulation steps, at least some depths should be nonzero
        assert arr.max() > 0.0

    def test_get_heads_bulk_after_steps(self, stepped_nodes):
        arr = stepped_nodes.get_heads_bulk()
        assert arr.shape == (stepped_nodes.count(),)


# ---------------------------------------------------------------------------
# All nodes iteration
# ---------------------------------------------------------------------------
class TestNodesIteration:
    """Iterate over all nodes and read properties."""

    def test_iterate_all_ids(self, nodes):
        n = nodes.count()
        ids = [nodes.get_id(i) for i in range(n)]
        assert len(ids) == n
        assert all(isinstance(name, str) and name for name in ids)

    def test_all_depths_non_negative(self, stepped_nodes):
        n = stepped_nodes.count()
        for i in range(n):
            d = stepped_nodes.get_depth(i)
            assert d >= 0.0, f"Node {i} has negative depth: {d}"


# ---------------------------------------------------------------------------
# String-based access
# ---------------------------------------------------------------------------
class TestNodesStringAccess:
    """Access node properties by string ID instead of integer index."""

    def test_get_depth_by_name(self, stepped_nodes):
        by_idx = stepped_nodes.get_depth(0)
        name = stepped_nodes.get_id(0)
        by_name = stepped_nodes.get_depth(name)
        assert by_idx == by_name

    def test_set_depth_by_name(self, nodes):
        name = nodes.get_id(0)
        nodes.set_depth(name, 2.5)
        assert abs(nodes.get_depth(name) - 2.5) < 1e-6

    def test_get_head_by_name(self, stepped_nodes):
        name = stepped_nodes.get_id(0)
        h = stepped_nodes.get_head(name)
        assert isinstance(h, float)

    def test_get_volume_by_name(self, stepped_nodes):
        name = stepped_nodes.get_id(0)
        v = stepped_nodes.get_volume(name)
        assert isinstance(v, float)

    def test_set_lateral_inflow_by_name(self, nodes):
        name = nodes.get_id(0)
        nodes.set_lateral_inflow(name, 0.5)

    def test_nonexistent_name_raises(self, nodes):
        with pytest.raises(KeyError, match="not found"):
            nodes.get_depth("NONEXISTENT_NODE")

    def test_all_nodes_accessible_by_name(self, stepped_nodes):
        n = stepped_nodes.count()
        for i in range(n):
            name = stepped_nodes.get_id(i)
            by_idx = stepped_nodes.get_depth(i)
            by_name = stepped_nodes.get_depth(name)
            assert by_idx == by_name, f"Mismatch for node '{name}'"
