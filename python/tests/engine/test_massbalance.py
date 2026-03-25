"""Tests for :class:`openswmm.engine.MassBalance` continuity error queries."""

import pytest

from openswmm.engine import MassBalance


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestMassBalanceConstruction:
    """MassBalance instantiation from a Solver."""

    def test_create_from_solver(self, completed_solver):
        mb = MassBalance(completed_solver)
        assert mb is not None


# ---------------------------------------------------------------------------
# Continuity errors
# ---------------------------------------------------------------------------
class TestMassBalanceContinuityErrors:
    """Query continuity errors after a completed simulation."""

    def test_runoff_continuity_error_type(self, mass_balance):
        err = mass_balance.get_runoff_continuity_error()
        assert isinstance(err, float)

    def test_runoff_continuity_error_reasonable(self, mass_balance):
        err = mass_balance.get_runoff_continuity_error()
        # Continuity error should be a small fraction (< 10%)
        assert abs(err) < 0.10

    def test_routing_continuity_error_type(self, mass_balance):
        err = mass_balance.get_routing_continuity_error()
        assert isinstance(err, float)

    def test_routing_continuity_error_reasonable(self, mass_balance):
        err = mass_balance.get_routing_continuity_error()
        assert abs(err) < 0.10

    def test_both_errors_available(self, mass_balance):
        r_err = mass_balance.get_runoff_continuity_error()
        q_err = mass_balance.get_routing_continuity_error()
        # Both should return without error
        assert isinstance(r_err, float)
        assert isinstance(q_err, float)
