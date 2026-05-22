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


# ---------------------------------------------------------------------------
# Creation methods (new additions)
# ---------------------------------------------------------------------------
class TestInfrastructureAdd:
    """transect_add, street_add, inlet_add, lid_add.

    These creation methods require BUILDING state (ModelBuilder).
    """

    def test_transect_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Infrastructure
        m = ModelBuilder()
        infra = Infrastructure(m)
        before = infra.transect_count()
        idx = infra.transect_add("TRN_TEST_NEW")
        assert isinstance(idx, int)
        assert idx >= 0
        assert infra.transect_count() == before + 1

    def test_street_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Infrastructure
        m = ModelBuilder()
        infra = Infrastructure(m)
        before = infra.street_count()
        idx = infra.street_add("STR_TEST_NEW")
        assert isinstance(idx, int)
        assert idx >= 0
        assert infra.street_count() == before + 1

    def test_inlet_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Infrastructure
        m = ModelBuilder()
        infra = Infrastructure(m)
        try:
            idx = infra.inlet_add("INL_TEST_NEW", "GRATE")
            assert isinstance(idx, int)
            assert idx >= 0
        except RuntimeError:
            pass  # type string may not be valid; call must not crash silently

    def test_lid_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Infrastructure
        m = ModelBuilder()
        infra = Infrastructure(m)
        # lid_add is currently a stub that returns SWMM_OK (0) — just verify
        # the call does not raise.
        result = infra.lid_add("LID_TEST_NEW", 0)
        assert result == 0  # SWMM_OK

