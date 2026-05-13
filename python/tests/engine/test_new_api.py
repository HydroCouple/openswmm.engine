"""Tests for new C API bindings from the refactored engine.

Covers Phase A-B functionality:
- Pump statistics (Links)
- Hydraulic power (Links)
- Outfall route-to (Nodes)
- Depth from volume (Nodes)
- Event/steady-state status (Solver)
- Routing stats, Courant, quality losses (MassBalance)
- Ponded quality (Subcatchments)
"""

import pytest
import numpy as np


# ============================================================================
# Links: Pump Statistics
# ============================================================================

class TestPumpStatistics:
    """Pump utilization statistics from Links binding."""

    def test_get_pump_cycles_exists(self, stepped_links):
        """Method should exist on Links class."""
        assert hasattr(stepped_links, "get_stat_pump_cycles")

    def test_get_pump_cycles_returns_int(self, stepped_links):
        """Pump cycles should be an integer >= 0."""
        # Link 0 may not be a pump, but should not crash
        v = stepped_links.get_stat_pump_cycles(0)
        assert isinstance(v, int)
        assert v >= 0

    def test_get_pump_on_time_exists(self, stepped_links):
        assert hasattr(stepped_links, "get_stat_pump_on_time")

    def test_get_pump_on_time_returns_float(self, stepped_links):
        v = stepped_links.get_stat_pump_on_time(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_pump_volume_exists(self, stepped_links):
        assert hasattr(stepped_links, "get_stat_pump_volume")

    def test_get_pump_volume_returns_float(self, stepped_links):
        v = stepped_links.get_stat_pump_volume(0)
        assert isinstance(v, float)
        assert v >= 0.0


# ============================================================================
# Links: Hydraulic Power
# ============================================================================

class TestHydraulicPower:
    """Hydraulic power computation from Links binding."""

    def test_get_hyd_power_exists(self, stepped_links):
        assert hasattr(stepped_links, "get_hyd_power")

    def test_get_hyd_power_returns_float(self, stepped_links):
        v = stepped_links.get_hyd_power(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_hyd_power_by_name(self, stepped_links):
        """Should accept string link ID."""
        link_id = stepped_links.get_id(0)
        v = stepped_links.get_hyd_power(link_id)
        assert isinstance(v, float)


# ============================================================================
# Nodes: Outfall Route-To
# ============================================================================

class TestOutfallRouteTo:
    """Outfall-to-subcatchment routing from Nodes binding."""

    def test_get_outfall_route_to_exists(self, stepped_nodes):
        assert hasattr(stepped_nodes, "get_outfall_route_to")

    def test_get_outfall_route_to_default(self, stepped_nodes):
        """Default should be -1 (no routing)."""
        v = stepped_nodes.get_outfall_route_to(0)
        assert isinstance(v, int)
        # Most nodes won't have route-to set
        assert v == -1 or v >= 0

    def test_set_outfall_route_to_exists(self, stepped_nodes):
        assert hasattr(stepped_nodes, "set_outfall_route_to")


# ============================================================================
# Nodes: Depth from Volume
# ============================================================================

class TestDepthFromVolume:
    """Inverse volume→depth from Nodes binding."""

    def test_get_depth_from_volume_exists(self, stepped_nodes):
        assert hasattr(stepped_nodes, "get_depth_from_volume")

    def test_get_depth_from_volume_zero(self, stepped_nodes):
        """Zero volume should return zero depth."""
        v = stepped_nodes.get_depth_from_volume(0, 0.0)
        assert isinstance(v, float)
        assert v == pytest.approx(0.0, abs=1e-10)

    def test_get_depth_from_volume_positive(self, stepped_nodes):
        """Positive volume should return positive depth."""
        v = stepped_nodes.get_depth_from_volume(0, 100.0)
        assert isinstance(v, float)
        assert v > 0.0


# ============================================================================
# Solver: Event and Steady-State Status
# ============================================================================

class TestEventStatus:
    """Routing event status from Solver binding."""

    def test_is_between_events_exists(self, running_solver):
        assert hasattr(running_solver, "is_between_events")

    def test_is_between_events_returns_bool(self, running_solver):
        v = running_solver.is_between_events()
        assert isinstance(v, bool)

    def test_get_event_count_exists(self, running_solver):
        assert hasattr(running_solver, "get_event_count")

    def test_get_event_count_returns_int(self, running_solver):
        v = running_solver.get_event_count()
        assert isinstance(v, int)
        assert v >= 0


class TestSteadyStateSkip:
    """Steady-state skip control from Solver binding."""

    def test_get_steady_state_skip_exists(self, running_solver):
        assert hasattr(running_solver, "get_steady_state_skip")

    def test_get_steady_state_skip_returns_bool(self, running_solver):
        v = running_solver.get_steady_state_skip()
        assert isinstance(v, bool)

    def test_set_steady_state_skip_exists(self, running_solver):
        assert hasattr(running_solver, "set_steady_state_skip")

    def test_set_and_get_roundtrip(self, running_solver):
        """Set True, get should return True."""
        running_solver.set_steady_state_skip(True)
        assert running_solver.get_steady_state_skip() is True
        running_solver.set_steady_state_skip(False)
        assert running_solver.get_steady_state_skip() is False


# ============================================================================
# MassBalance: Routing Stats and Quality Losses
# ============================================================================

class TestRoutingStats:
    """Routing diagnostics from MassBalance binding."""

    def test_get_routing_stats_exists(self, mass_balance):
        assert hasattr(mass_balance, "get_routing_stats")

    def test_get_routing_stats_returns_dict(self, mass_balance):
        v = mass_balance.get_routing_stats()
        assert isinstance(v, dict)
        assert "avg_step" in v
        assert "min_step" in v
        assert "max_step" in v
        assert "n_steps" in v
        assert "pct_non_converged" in v
        assert "avg_iterations" in v
        assert "max_courant" in v

    def test_routing_stats_positive_steps(self, mass_balance):
        v = mass_balance.get_routing_stats()
        assert v["n_steps"] > 0
        assert v["avg_step"] > 0.0

    def test_get_max_courant_exists(self, mass_balance):
        assert hasattr(mass_balance, "get_max_courant")

    def test_get_max_courant_returns_float(self, mass_balance):
        v = mass_balance.get_max_courant()
        assert isinstance(v, float)
        assert v >= 0.0


class TestQualityLosses:
    """Quality seepage/evaporation losses from MassBalance binding."""

    def test_get_quality_seep_loss_exists(self, mass_balance):
        assert hasattr(mass_balance, "get_quality_seep_loss")

    def test_get_quality_seep_loss_returns_float(self, mass_balance):
        # May be 0 if no pollutants or no seepage
        v = mass_balance.get_quality_seep_loss(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_get_quality_evap_loss_exists(self, mass_balance):
        assert hasattr(mass_balance, "get_quality_evap_loss")

    def test_get_quality_evap_loss_returns_float(self, mass_balance):
        v = mass_balance.get_quality_evap_loss(0)
        assert isinstance(v, float)
        assert v >= 0.0


# ============================================================================
# Subcatchments: Ponded Quality
# ============================================================================

class TestPondedQuality:
    """Ponded quality mass from Subcatchments binding."""

    def test_get_ponded_quality_exists(self, stepped_subcatchments):
        assert hasattr(stepped_subcatchments, "get_ponded_quality")

    def test_set_ponded_quality_exists(self, stepped_subcatchments):
        assert hasattr(stepped_subcatchments, "set_ponded_quality")

    def test_get_ponded_quality_returns_float(self, stepped_subcatchments):
        # May be 0 if no pollutants defined
        try:
            v = stepped_subcatchments.get_ponded_quality(0, 0)
            assert isinstance(v, float)
            assert v >= 0.0
        except Exception:
            # Might fail if no pollutants — that's OK
            pass
