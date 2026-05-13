"""Tests for :class:`openswmm.engine.Infrastructure` (transects, streets, inlets, LIDs)."""

import pytest


class TestInfrastructureCounts:
    """Infrastructure count queries."""

    def test_transect_count(self, running_solver):
        from openswmm.engine import Infrastructure
        infra = Infrastructure(running_solver)
        n = infra.transect_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_street_count(self, running_solver):
        from openswmm.engine import Infrastructure
        infra = Infrastructure(running_solver)
        n = infra.street_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_inlet_count(self, running_solver):
        from openswmm.engine import Infrastructure
        infra = Infrastructure(running_solver)
        n = infra.inlet_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_lid_count(self, running_solver):
        from openswmm.engine import Infrastructure
        infra = Infrastructure(running_solver)
        n = infra.lid_count()
        assert isinstance(n, int)
        assert n >= 0
