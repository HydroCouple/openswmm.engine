"""Tests for :class:`openswmm.engine.Pollutants` and :class:`openswmm.engine.Quality`."""

import pytest


class TestPollutantsCount:
    """Pollutant queries."""

    def test_count(self, running_solver):
        from openswmm.engine import Pollutants
        p = Pollutants(running_solver)
        n = p.count()
        assert isinstance(n, int)
        assert n >= 0


class TestQualityLanduse:
    """Quality / landuse queries."""

    def test_landuse_count(self, running_solver):
        from openswmm.engine import Quality
        q = Quality(running_solver)
        n = q.landuse_count()
        assert isinstance(n, int)
        assert n >= 0


# ---------------------------------------------------------------------------
# landuse_add (new addition)
# ---------------------------------------------------------------------------
class TestQualityLanduseAdd:
    """landuse_add creates a new land use entry.

    Requires BUILDING state (ModelBuilder).
    """

    def test_landuse_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Quality
        m = ModelBuilder()
        q = Quality(m)
        before = q.landuse_count()
        idx = q.landuse_add("LU_TEST_NEW")
        assert isinstance(idx, int)
        assert idx >= 0
        assert q.landuse_count() == before + 1

