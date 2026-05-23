"""Tests for :class:`openswmm.engine.Subcatchments` access during simulation."""

import pytest

from openswmm.engine import Subcatchments


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestSubcatchmentsConstruction:
    """Subcatchments instantiation from a Solver."""

    def test_create_from_solver(self, running_solver):
        sc = Subcatchments(running_solver)
        assert sc is not None

    def test_count(self, subcatchments):
        n = subcatchments.count()
        assert isinstance(n, int)
        assert n == 7  # site-drainage example has 7 subcatchments


# ---------------------------------------------------------------------------
# Index / ID lookup
# ---------------------------------------------------------------------------
class TestSubcatchmentsLookup:
    """Name to index resolution."""

    def test_get_index_valid(self, subcatchments):
        idx = subcatchments.get_index("S1")
        assert isinstance(idx, int)
        assert idx >= 0

    def test_get_index_not_found(self, subcatchments):
        idx = subcatchments.get_index("NONEXISTENT")
        assert idx == -1

    def test_roundtrip_index(self, subcatchments):
        idx = subcatchments.get_index("S1")
        assert idx >= 0


# ---------------------------------------------------------------------------
# Getters
# ---------------------------------------------------------------------------
class TestSubcatchmentsGetters:
    """Read subcatchment properties."""

    def test_get_runoff_initial(self, subcatchments):
        r = subcatchments.get_runoff(0)
        assert isinstance(r, float)
        assert r >= 0.0

    def test_get_runoff_after_steps(self, stepped_subcatchments):
        r = stepped_subcatchments.get_runoff(0)
        assert isinstance(r, float)

    def test_get_groundwater(self, stepped_subcatchments):
        gw = stepped_subcatchments.get_groundwater(0)
        assert isinstance(gw, float)
        assert gw >= 0.0

    def test_all_subcatchments_runoff(self, stepped_subcatchments):
        n = stepped_subcatchments.count()
        for i in range(n):
            r = stepped_subcatchments.get_runoff(i)
            assert isinstance(r, float)


# ---------------------------------------------------------------------------
# Setters
# ---------------------------------------------------------------------------
class TestSubcatchmentsSetters:
    """Write subcatchment properties."""

    def test_set_rainfall_positive(self, subcatchments):
        # Override rainfall to a positive value -- should not raise
        subcatchments.set_rainfall(0, 2.5)

    def test_set_rainfall_zero(self, subcatchments):
        subcatchments.set_rainfall(0, 0.0)

    def test_set_rainfall_negative_reverts(self, subcatchments):
        # Negative value should revert to gage-driven rainfall
        subcatchments.set_rainfall(0, -1.0)

    def test_set_rainfall_each_subcatchment(self, subcatchments):
        n = subcatchments.count()
        for i in range(n):
            subcatchments.set_rainfall(i, 1.0)


# ---------------------------------------------------------------------------
# String-based access
# ---------------------------------------------------------------------------
class TestSubcatchmentsStringAccess:
    """Access subcatchment properties by string ID."""

    def test_get_runoff_by_name(self, stepped_subcatchments):
        by_idx = stepped_subcatchments.get_runoff(0)
        name = "S1"
        by_name = stepped_subcatchments.get_runoff(name)
        assert by_idx == by_name

    def test_get_groundwater_by_name(self, stepped_subcatchments):
        gw = stepped_subcatchments.get_groundwater("S1")
        assert isinstance(gw, float)

    def test_set_rainfall_by_name(self, subcatchments):
        subcatchments.set_rainfall("S1", 2.5)

    def test_nonexistent_name_raises(self, subcatchments):
        with pytest.raises(KeyError, match="not found"):
            subcatchments.get_runoff("NONEXISTENT_SC")
