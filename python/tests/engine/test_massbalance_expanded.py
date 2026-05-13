"""Tests for expanded :class:`openswmm.engine.MassBalance` bindings."""

import pytest

from openswmm.engine import MassBalance


class TestMassBalanceTotals:
    """Cumulative mass balance totals."""

    def test_get_runoff_total_rainfall(self, mass_balance):
        """Runoff total component 0 = RAINFALL."""
        v = mass_balance.get_runoff_total(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_runoff_total_evap(self, mass_balance):
        """Runoff total component 1 = EVAP."""
        v = mass_balance.get_runoff_total(1)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_runoff_total_infil(self, mass_balance):
        """Runoff total component 2 = INFIL."""
        v = mass_balance.get_runoff_total(2)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_runoff_total_runoff(self, mass_balance):
        """Runoff total component 3 = RUNOFF."""
        v = mass_balance.get_runoff_total(3)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_routing_total_wet_weather(self, mass_balance):
        """Routing total component 1 = WET_WEATHER."""
        v = mass_balance.get_routing_total(1)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_routing_total_outflow(self, mass_balance):
        """Routing total component 6 = OUTFLOW."""
        v = mass_balance.get_routing_total(6)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_routing_mass_balance_closure(self, mass_balance):
        """Routing inflows - outflows ~ storage change."""
        total_inflow = sum(mass_balance.get_routing_total(i) for i in range(5))
        flooding = mass_balance.get_routing_total(5)
        outflow = mass_balance.get_routing_total(6)
        evap_loss = mass_balance.get_routing_total(7)
        seep_loss = mass_balance.get_routing_total(8)
        init_storage = mass_balance.get_routing_total(9)
        final_storage = mass_balance.get_routing_total(10)

        total_outflow = flooding + outflow + evap_loss + seep_loss
        storage_change = final_storage - init_storage
        balance = total_inflow - total_outflow - storage_change

        if total_inflow > 0:
            pct_error = abs(balance) / total_inflow
            assert pct_error < 0.05, f"Mass balance error {pct_error:.2%}"
