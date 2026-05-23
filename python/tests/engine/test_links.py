"""Tests for :class:`openswmm.engine.Links` link access during simulation."""

import numpy as np
import pytest

from openswmm.engine import Links


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestLinksConstruction:
    """Links instantiation from a Solver."""

    def test_create_from_solver(self, running_solver):
        links = Links(running_solver)
        assert links is not None

    def test_count(self, links):
        n = links.count()
        assert isinstance(n, int)
        assert n == 11  # site-drainage example has 11 links


# ---------------------------------------------------------------------------
# Index / ID lookup
# ---------------------------------------------------------------------------
class TestLinksLookup:
    """Name to index resolution."""

    def test_get_index_valid(self, links):
        idx = links.get_index("C1")
        assert isinstance(idx, int)
        assert idx >= 0

    def test_get_index_not_found(self, links):
        idx = links.get_index("NONEXISTENT")
        assert idx == -1

    def test_get_id(self, links):
        name = links.get_id(0)
        assert isinstance(name, str)
        assert len(name) > 0

    def test_roundtrip_id_index(self, links):
        name = "C1"
        idx = links.get_index(name)
        assert links.get_id(idx) == name


# ---------------------------------------------------------------------------
# Getters
# ---------------------------------------------------------------------------
class TestLinksGetters:
    """Individual link property reads."""

    def test_get_flow_initial(self, links):
        f = links.get_flow(0)
        assert isinstance(f, float)

    def test_get_flow_after_steps(self, stepped_links):
        f = stepped_links.get_flow(0)
        assert isinstance(f, float)

    def test_get_depth(self, stepped_links):
        d = stepped_links.get_depth(0)
        assert isinstance(d, float)
        assert d >= 0.0

    def test_get_control_setting(self, links):
        s = links.get_control_setting(0)
        assert isinstance(s, float)
        assert 0.0 <= s <= 1.0


# ---------------------------------------------------------------------------
# Setters
# ---------------------------------------------------------------------------
class TestLinksSetters:
    """Individual link property writes."""

    def test_set_flow(self, links):
        links.set_flow(0, 5.0)
        f = links.get_flow(0)
        assert abs(f - 5.0) < 1e-6

    def test_set_control_setting(self, links):
        links.set_control_setting(0, 0.75)
        s = links.get_control_setting(0)
        assert abs(s - 0.75) < 1e-6

    def test_set_control_setting_closed(self, links):
        links.set_control_setting(0, 0.0)
        s = links.get_control_setting(0)
        assert abs(s) < 1e-6

    def test_set_control_setting_fully_open(self, links):
        links.set_control_setting(0, 1.0)
        s = links.get_control_setting(0)
        assert abs(s - 1.0) < 1e-6


# ---------------------------------------------------------------------------
# Bulk operations
# ---------------------------------------------------------------------------
class TestLinksBulk:
    """Bulk array get/set using NumPy."""

    def test_get_flows_bulk_shape(self, links):
        arr = links.get_flows_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape == (links.count(),)

    def test_set_flows_bulk(self, links):
        n = links.count()
        new_flows = np.full(n, 3.0, dtype=np.float64)
        links.set_flows_bulk(new_flows)
        result = links.get_flows_bulk()
        np.testing.assert_allclose(result, new_flows, atol=1e-10)

    def test_get_flows_bulk_after_steps(self, stepped_links):
        arr = stepped_links.get_flows_bulk()
        # After simulation steps, at least some flows should be nonzero
        assert np.abs(arr).max() > 0.0


# ---------------------------------------------------------------------------
# Iteration
# ---------------------------------------------------------------------------
class TestLinksIteration:
    """Iterate over all links."""

    def test_iterate_all_ids(self, links):
        n = links.count()
        ids = [links.get_id(i) for i in range(n)]
        assert len(ids) == n
        assert all(isinstance(name, str) and name for name in ids)


# ---------------------------------------------------------------------------
# String-based access
# ---------------------------------------------------------------------------
class TestLinksStringAccess:
    """Access link properties by string ID instead of integer index."""

    def test_get_flow_by_name(self, stepped_links):
        by_idx = stepped_links.get_flow(0)
        name = stepped_links.get_id(0)
        by_name = stepped_links.get_flow(name)
        assert by_idx == by_name

    def test_set_flow_by_name(self, links):
        name = links.get_id(0)
        links.set_flow(name, 5.0)
        assert abs(links.get_flow(name) - 5.0) < 1e-6

    def test_get_depth_by_name(self, stepped_links):
        name = stepped_links.get_id(0)
        d = stepped_links.get_depth(name)
        assert isinstance(d, float)

    def test_get_control_setting_by_name(self, links):
        name = links.get_id(0)
        s = links.get_control_setting(name)
        assert isinstance(s, float)

    def test_set_control_setting_by_name(self, links):
        name = links.get_id(0)
        links.set_control_setting(name, 0.5)
        assert abs(links.get_control_setting(name) - 0.5) < 1e-6

    def test_nonexistent_name_raises(self, links):
        with pytest.raises(KeyError, match="not found"):
            links.get_flow("NONEXISTENT_LINK")

    def test_all_links_accessible_by_name(self, stepped_links):
        n = stepped_links.count()
        for i in range(n):
            name = stepped_links.get_id(i)
            assert stepped_links.get_flow(i) == stepped_links.get_flow(name)
