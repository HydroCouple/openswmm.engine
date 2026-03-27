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
