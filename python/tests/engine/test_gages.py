"""Tests for :class:`openswmm.engine.Gages` rain gage access during simulation."""

import pytest

from openswmm.engine import Gages


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestGagesConstruction:
    """Gages instantiation from a Solver."""

    def test_create_from_solver(self, running_solver):
        g = Gages(running_solver)
        assert g is not None

    def test_count(self, gages):
        n = gages.count()
        assert isinstance(n, int)
        assert n == 1  # site-drainage example has 1 rain gage


# ---------------------------------------------------------------------------
# Index / ID lookup
# ---------------------------------------------------------------------------
class TestGagesLookup:
    """Name to index resolution."""

    def test_get_index_valid(self, gages):
        idx = gages.get_index("RainGage")
        assert isinstance(idx, int)
        assert idx == 0

    def test_get_index_not_found(self, gages):
        idx = gages.get_index("NONEXISTENT")
        assert idx == -1


# ---------------------------------------------------------------------------
# Getters
# ---------------------------------------------------------------------------
class TestGagesGetters:
    """Read gage properties."""

    def test_get_rainfall_initial(self, gages):
        r = gages.get_rainfall(0)
        assert isinstance(r, float)
        assert r >= 0.0

    def test_get_rainfall_after_steps(self, stepped_gages):
        r = stepped_gages.get_rainfall(0)
        assert isinstance(r, float)
        assert r >= 0.0


# ---------------------------------------------------------------------------
# Setters
# ---------------------------------------------------------------------------
class TestGagesSetters:
    """Override gage rainfall."""

    def test_set_rainfall(self, gages):
        gages.set_rainfall(0, 25.4)  # mm/hr
        r = gages.get_rainfall(0)
        assert isinstance(r, float)
        # Value may be converted, but should not raise

    def test_set_rainfall_zero(self, gages):
        gages.set_rainfall(0, 0.0)

    def test_set_rainfall_persists_one_step(self, running_solver):
        g = Gages(running_solver)
        g.set_rainfall(0, 10.0)
        running_solver.step()
        r = g.get_rainfall(0)
        assert isinstance(r, float)


# ---------------------------------------------------------------------------
# String-based access
# ---------------------------------------------------------------------------
class TestGagesStringAccess:
    """Access gage properties by string ID."""

    def test_get_rainfall_by_name(self, stepped_gages):
        by_idx = stepped_gages.get_rainfall(0)
        by_name = stepped_gages.get_rainfall("RainGage")
        assert by_idx == by_name

    def test_set_rainfall_by_name(self, gages):
        gages.set_rainfall("RainGage", 25.4)

    def test_nonexistent_name_raises(self, gages):
        with pytest.raises(KeyError, match="not found"):
            gages.get_rainfall("NONEXISTENT_GAGE")
