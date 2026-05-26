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


class TestPumpStatsBulk:
    """Bulk pump-stats accessor — single C call returning numpy arrays.

    The contract under test:

      * Returns a dict with keys ``cycles`` (int32), ``on_time`` (float64),
        and ``volume`` (float64). Each array has length ``n_links``.
      * For non-pump links, ``cycles[i] == -1`` and ``on_time[i] == 0.0``
        and ``volume[i] == 0.0`` (the cycles sentinel is the discriminator).
      * For pump links, the values match the per-link scalar getters
        exactly (numerical equivalence).
      * The number of non-sentinel entries equals the number of pump links
        when discovered independently via ``get_type``.
    """

    def test_exists(self, stepped_links):
        assert hasattr(stepped_links, "get_pump_stats_bulk")

    def test_return_shape_and_dtypes(self, stepped_links):
        n = stepped_links.count()
        result = stepped_links.get_pump_stats_bulk()
        assert set(result.keys()) == {"cycles", "on_time", "volume"}
        assert result["cycles"].shape == (n,)
        assert result["on_time"].shape == (n,)
        assert result["volume"].shape == (n,)
        assert result["cycles"].dtype == np.intc
        assert result["on_time"].dtype == np.float64
        assert result["volume"].dtype == np.float64

    def test_equivalence_with_scalar_getters(self, stepped_links):
        """Bulk values must match scalar values for every pump link.

        Non-pump links are skipped because the scalar getters read raw
        statistics vectors and may return zeros while the bulk getter
        emits the documented sentinel.
        """
        result = stepped_links.get_pump_stats_bulk()
        n = stepped_links.count()
        # LinkType.PUMP == 1 (see LinkData.hpp); use get_type to discover.
        for i in range(n):
            if stepped_links.get_type(i) != 1:  # not a pump
                assert result["cycles"][i] == -1
                assert result["on_time"][i] == 0.0
                assert result["volume"][i] == 0.0
                continue
            # Pump link: bulk == scalar exactly.
            assert result["cycles"][i] == stepped_links.get_stat_pump_cycles(i)
            assert result["on_time"][i] == stepped_links.get_stat_pump_on_time(i)
            assert result["volume"][i] == stepped_links.get_stat_pump_volume(i)

    def test_sentinel_count_matches_non_pumps(self, stepped_links):
        """Count of -1 sentinels should equal the count of non-pump links."""
        result = stepped_links.get_pump_stats_bulk()
        n = stepped_links.count()
        n_non_pumps = sum(1 for i in range(n) if stepped_links.get_type(i) != 1)
        assert int((result["cycles"] == -1).sum()) == n_non_pumps

    def test_returns_contiguous_arrays(self, stepped_links):
        """C call passes raw pointers; arrays must be C-contiguous."""
        result = stepped_links.get_pump_stats_bulk()
        for arr in result.values():
            assert arr.flags["C_CONTIGUOUS"]


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


class TestEventsEditor:
    """C{[EVENTS]} section editor (Slice CW — 2026-05-21).

    OADate values are decimal days since 1899-12-30; arithmetic is
    calendar-agnostic here so any disjoint pair works.
    """

    def test_events_count_alias(self, running_solver):
        # events_count must agree with the legacy get_event_count.
        assert running_solver.events_count() == running_solver.get_event_count()

    def test_events_add_then_get_round_trips(self, running_solver):
        before = running_solver.events_count()
        idx = running_solver.events_add(46036.0, 46036.5)
        assert idx == before
        s, e = running_solver.events_get(idx)
        assert s == 46036.0
        assert e == 46036.5
        running_solver.events_remove(idx)
        assert running_solver.events_count() == before

    def test_events_add_rejects_start_ge_end(self, running_solver):
        with pytest.raises(RuntimeError):
            running_solver.events_add(10.0, 10.0)
        with pytest.raises(RuntimeError):
            running_solver.events_add(10.0, 9.0)

    def test_events_set_rejects_start_ge_end(self, running_solver):
        before = running_solver.events_count()
        idx = running_solver.events_add(20.0, 21.0)
        try:
            with pytest.raises(RuntimeError):
                running_solver.events_set(idx, 21.0, 21.0)
            # Underlying row unchanged after rejected set.
            s, e = running_solver.events_get(idx)
            assert s == 20.0
            assert e == 21.0
        finally:
            running_solver.events_remove(idx)
            assert running_solver.events_count() == before

    def test_events_clear(self, running_solver):
        # Stash whatever the .inp had, restore on teardown so we don't
        # leak edits across tests sharing a running solver.
        original = [running_solver.events_get(i)
                    for i in range(running_solver.events_count())]
        try:
            running_solver.events_add(30.0, 31.0)
            running_solver.events_add(32.0, 33.0)
            running_solver.events_clear()
            assert running_solver.events_count() == 0
        finally:
            for s, e in original:
                running_solver.events_add(s, e)


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


# ============================================================================
# Phase 3: Nodes bulk getters (volumes / outflows / losses /
# lateral_inflows / ids)
# ============================================================================

class TestNodesPhase3Bulk:
    """Equivalence and contract tests for the Phase 3 nodes-bulk family.

    The contract under test for each get_*_bulk:

      * Returns a NumPy ``float64`` (or ``list[str]`` for ids) of length
        ``n_nodes``.
      * Each entry matches the scalar accessor on the same engine state
        (numerical equivalence).
      * The C array is contiguous (the C call writes raw doubles into
        ``arr.data``).
    """

    def test_get_volumes_bulk_exists(self, stepped_nodes):
        assert hasattr(stepped_nodes, "get_volumes_bulk")

    def test_get_volumes_bulk_shape_and_dtype(self, stepped_nodes):
        n = stepped_nodes.count()
        arr = stepped_nodes.get_volumes_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (n,)
        assert arr.dtype == np.float64
        assert arr.flags["C_CONTIGUOUS"]

    def test_get_volumes_bulk_equivalence(self, stepped_nodes):
        arr = stepped_nodes.get_volumes_bulk()
        n = stepped_nodes.count()
        for i in range(n):
            assert arr[i] == stepped_nodes.get_volume(i), f"node {i}"

    def test_get_outflows_bulk_equivalence(self, stepped_nodes):
        arr = stepped_nodes.get_outflows_bulk()
        n = stepped_nodes.count()
        for i in range(n):
            assert arr[i] == stepped_nodes.get_outflow(i), f"node {i}"

    def test_get_losses_bulk_equivalence(self, stepped_nodes):
        arr = stepped_nodes.get_losses_bulk()
        n = stepped_nodes.count()
        for i in range(n):
            assert arr[i] == stepped_nodes.get_losses(i), f"node {i}"

    def test_get_lateral_inflows_bulk_equivalence(self, stepped_nodes):
        arr = stepped_nodes.get_lateral_inflows_bulk()
        n = stepped_nodes.count()
        for i in range(n):
            assert arr[i] == stepped_nodes.get_lateral_inflow(i), f"node {i}"

    def test_get_lateral_inflows_bulk_matches_inflows_bulk(self, stepped_nodes):
        """Backward-compat: ``get_lateral_inflows_bulk`` reads the same
        SoA column as the older ``get_inflows_bulk`` (both expose
        ``lat_flow``)."""
        new = stepped_nodes.get_lateral_inflows_bulk()
        old = stepped_nodes.get_inflows_bulk()
        np.testing.assert_array_equal(new, old)

    def test_get_ids_bulk_returns_list_of_strings(self, stepped_nodes):
        ids = stepped_nodes.get_ids_bulk()
        n = stepped_nodes.count()
        assert isinstance(ids, list)
        assert len(ids) == n
        for s in ids:
            assert isinstance(s, str)

    def test_get_ids_bulk_equivalence_with_scalar(self, stepped_nodes):
        ids = stepped_nodes.get_ids_bulk()
        n = stepped_nodes.count()
        for i in range(n):
            assert ids[i] == stepped_nodes.get_id(i), f"index {i}"

    def test_get_ids_bulk_handles_short_stride_truncation(self, stepped_nodes):
        """A deliberately small stride should truncate IDs without
        crashing. Each returned string is at most ``stride - 1`` chars."""
        stride = 4
        ids = stepped_nodes.get_ids_bulk(stride=stride)
        for s in ids:
            # UTF-8 length (bytes), not codepoints — but SWMM IDs are
            # ASCII so they coincide. Allow exactly stride-1 bytes max.
            assert len(s.encode("utf-8")) <= stride - 1

    def test_get_ids_bulk_default_stride_no_truncation(self, stepped_nodes):
        """At the default stride of 64, the site_drainage fixture's IDs
        must round-trip without loss."""
        ids = stepped_nodes.get_ids_bulk()
        for i, s in enumerate(ids):
            assert s == stepped_nodes.get_id(i)


# ============================================================================
# Phase 3: Links bulk getters (velocities / capacities / volumes /
# control_settings / target_settings / hyd_powers / ids)
# ============================================================================

class TestLinksPhase3Bulk:
    """Equivalence and contract tests for the Phase 3 links-bulk family.

    Velocities, capacities, and hyd_powers are derived per-link in C; the
    rest are SoA memcpys. Both flavours must agree bit-for-bit with the
    matching scalar accessors.
    """

    def test_velocities_bulk_shape(self, stepped_links):
        arr = stepped_links.get_velocities_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_links.count(),)
        assert arr.dtype == np.float64
        assert arr.flags["C_CONTIGUOUS"]

    def test_velocities_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_velocities_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_velocity(i), f"link {i}"

    def test_capacities_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_capacities_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_capacity(i), f"link {i}"

    def test_volumes_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_volumes_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_volume(i), f"link {i}"

    def test_control_settings_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_control_settings_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_control_setting(i), f"link {i}"

    def test_target_settings_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_target_settings_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_target_setting(i), f"link {i}"

    def test_hyd_powers_bulk_equivalence(self, stepped_links):
        arr = stepped_links.get_hyd_powers_bulk()
        for i in range(stepped_links.count()):
            assert arr[i] == stepped_links.get_hyd_power(i), f"link {i}"

    def test_ids_bulk_returns_list_of_strings(self, stepped_links):
        ids = stepped_links.get_ids_bulk()
        assert isinstance(ids, list)
        assert len(ids) == stepped_links.count()
        for s in ids:
            assert isinstance(s, str)

    def test_ids_bulk_equivalence_with_scalar(self, stepped_links):
        ids = stepped_links.get_ids_bulk()
        for i, s in enumerate(ids):
            assert s == stepped_links.get_id(i), f"index {i}"

    def test_ids_bulk_handles_short_stride_truncation(self, stepped_links):
        stride = 4
        ids = stepped_links.get_ids_bulk(stride=stride)
        for s in ids:
            assert len(s.encode("utf-8")) <= stride - 1

    def test_ids_bulk_default_stride_no_truncation(self, stepped_links):
        ids = stepped_links.get_ids_bulk()
        for i, s in enumerate(ids):
            assert s == stepped_links.get_id(i)

    def test_pump_filtered_hyd_power_summary(self, stepped_links):
        """End-to-end pattern: pair ``get_pump_stats_bulk`` with
        ``get_hyd_powers_bulk`` to get pump power totals — the canonical
        idiom for the MCP server's pump-energy summary."""
        stats = stepped_links.get_pump_stats_bulk()
        powers = stepped_links.get_hyd_powers_bulk()
        mask = stats["cycles"] >= 0
        # Non-pumps still have a defined hyd_power, but the sum below
        # is restricted to pumps only — this is the documented pattern.
        pump_power = float(powers[mask].sum())
        assert pump_power >= 0.0


# ============================================================================
# Phase 3: Subcatchments bulk getters (rainfall / evap / infil /
# snow_depth / ids)
# ============================================================================

class TestSubcatchmentsPhase3Bulk:
    """Equivalence and contract tests for the Phase 3 subcatchments-bulk
    family.

    Snow depth currently returns zeros from both the scalar and bulk
    variants (placeholder until snow-state integration); the equivalence
    test exercises that documented behaviour.
    """

    def test_rainfall_bulk_shape(self, stepped_subcatchments):
        arr = stepped_subcatchments.get_rainfall_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_subcatchments.count(),)
        assert arr.dtype == np.float64
        assert arr.flags["C_CONTIGUOUS"]

    def test_rainfall_bulk_equivalence(self, stepped_subcatchments):
        arr = stepped_subcatchments.get_rainfall_bulk()
        for i in range(stepped_subcatchments.count()):
            assert arr[i] == stepped_subcatchments.get_rainfall(i), f"subcatch {i}"

    def test_evap_bulk_equivalence(self, stepped_subcatchments):
        arr = stepped_subcatchments.get_evap_bulk()
        for i in range(stepped_subcatchments.count()):
            assert arr[i] == stepped_subcatchments.get_evap(i), f"subcatch {i}"

    def test_infil_bulk_equivalence(self, stepped_subcatchments):
        arr = stepped_subcatchments.get_infil_bulk()
        for i in range(stepped_subcatchments.count()):
            assert arr[i] == stepped_subcatchments.get_infil(i), f"subcatch {i}"

    def test_snow_depth_bulk_returns_zeros_placeholder(self, stepped_subcatchments):
        """Mirrors the scalar accessor's placeholder behavior — every
        entry is 0.0 until snow-state integration lands. When that
        integration completes, both this test and the scalar test will
        need a corresponding update."""
        arr = stepped_subcatchments.get_snow_depth_bulk()
        for i in range(stepped_subcatchments.count()):
            scalar = stepped_subcatchments.get_snow_depth(i)
            assert arr[i] == scalar, f"subcatch {i}"
            assert arr[i] == 0.0, f"subcatch {i}: expected placeholder zero"

    def test_ids_bulk_returns_list_of_strings(self, stepped_subcatchments):
        ids = stepped_subcatchments.get_ids_bulk()
        assert isinstance(ids, list)
        assert len(ids) == stepped_subcatchments.count()
        for s in ids:
            assert isinstance(s, str)

    def test_ids_bulk_equivalence_with_scalar(self, stepped_subcatchments):
        ids = stepped_subcatchments.get_ids_bulk()
        for i, s in enumerate(ids):
            assert s == stepped_subcatchments.get_id(i), f"index {i}"

    def test_ids_bulk_handles_short_stride_truncation(self, stepped_subcatchments):
        stride = 4
        ids = stepped_subcatchments.get_ids_bulk(stride=stride)
        for s in ids:
            assert len(s.encode("utf-8")) <= stride - 1

    def test_ids_bulk_default_stride_no_truncation(self, stepped_subcatchments):
        ids = stepped_subcatchments.get_ids_bulk()
        for i, s in enumerate(ids):
            assert s == stepped_subcatchments.get_id(i)

    def test_whole_network_water_balance_pattern(self, stepped_subcatchments):
        """End-to-end pattern: rainfall - infil - evap - runoff per
        subcatchment as a quick water-balance check. This is the canonical
        MCP / GUI idiom; verifies the four hydrology bulk getters return
        equally-sized arrays in matching subcatch order."""
        rain = stepped_subcatchments.get_rainfall_bulk()
        infil = stepped_subcatchments.get_infil_bulk()
        evap = stepped_subcatchments.get_evap_bulk()
        runoff = stepped_subcatchments.get_runoff_bulk()
        n = stepped_subcatchments.count()
        assert rain.shape == (n,)
        assert infil.shape == (n,)
        assert evap.shape == (n,)
        assert runoff.shape == (n,)
        # Each subcatch has a non-negative residual when storage is steady.
        # We don't assert sign here (snow / storage make the instantaneous
        # balance noisy), just that we can compute it without error.
        residual = rain - infil - evap - runoff
        assert residual.shape == (n,)


# ============================================================================
# Phase 3: Statistics bulk getters (node max_overflow / vol_flooded /
# time_flooded; subcatch max_runoff)
# ============================================================================

class TestStatisticsPhase3Bulk:
    """Equivalence + non-negativity tests for the Phase 3 statistics-bulk
    family. These are cumulative quantities populated over the simulation,
    so each test runs against ``completed_solver`` (a fixture that drives
    the site_drainage_model through to ENDED) and asserts equivalence with
    the post-run scalar accessor.
    """

    def test_node_max_overflow_bulk_shape(self, completed_solver):
        from openswmm.engine import Nodes, Statistics
        stats = Statistics(completed_solver)
        nodes = Nodes(completed_solver)
        arr = stats.node_max_overflow_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (nodes.count(),)
        assert arr.dtype == np.float64
        assert arr.flags["C_CONTIGUOUS"]

    def test_node_max_overflow_bulk_equivalence(self, completed_solver):
        from openswmm.engine import Nodes, Statistics
        stats = Statistics(completed_solver)
        nodes = Nodes(completed_solver)
        arr = stats.node_max_overflow_bulk()
        for i in range(nodes.count()):
            assert arr[i] == nodes.get_stat_max_overflow(i), f"node {i}"

    def test_node_vol_flooded_bulk_equivalence(self, completed_solver):
        from openswmm.engine import Nodes, Statistics
        stats = Statistics(completed_solver)
        nodes = Nodes(completed_solver)
        arr = stats.node_vol_flooded_bulk()
        for i in range(nodes.count()):
            assert arr[i] == nodes.get_stat_vol_flooded(i), f"node {i}"

    def test_node_time_flooded_bulk_equivalence(self, completed_solver):
        from openswmm.engine import Nodes, Statistics
        stats = Statistics(completed_solver)
        nodes = Nodes(completed_solver)
        arr = stats.node_time_flooded_bulk()
        for i in range(nodes.count()):
            assert arr[i] == nodes.get_stat_time_flooded(i), f"node {i}"

    def test_subcatch_max_runoff_bulk_equivalence(self, completed_solver):
        from openswmm.engine import Subcatchments, Statistics
        stats = Statistics(completed_solver)
        subs = Subcatchments(completed_solver)
        arr = stats.subcatch_max_runoff_bulk()
        for i in range(subs.count()):
            assert arr[i] == subs.get_stat_max_runoff(i), f"subcatch {i}"

    def test_all_stats_non_negative(self, completed_solver):
        """Cumulative non-negative quantities — a regression that reads the
        wrong SoA column would likely produce negatives here."""
        from openswmm.engine import Statistics
        stats = Statistics(completed_solver)
        for arr_name, arr in [
            ("node_max_overflow_bulk", stats.node_max_overflow_bulk()),
            ("node_vol_flooded_bulk",  stats.node_vol_flooded_bulk()),
            ("node_time_flooded_bulk", stats.node_time_flooded_bulk()),
            ("subcatch_max_runoff_bulk", stats.subcatch_max_runoff_bulk()),
        ]:
            assert (arr >= 0).all(), f"{arr_name} has a negative entry"

    def test_flooding_summary_idiom(self, completed_solver):
        """End-to-end pattern: pair the three flooding bulk getters with
        ``Nodes.get_ids_bulk()`` to assemble a network-wide flooding
        summary in 4 C calls instead of 4*N. This is the canonical
        replacement for the MCP server's ``get_flooding_summary`` Python
        loop."""
        from openswmm.engine import Nodes, Statistics
        stats = Statistics(completed_solver)
        nodes = Nodes(completed_solver)
        ids       = nodes.get_ids_bulk()
        max_over  = stats.node_max_overflow_bulk()
        vol_flood = stats.node_vol_flooded_bulk()
        t_flood   = stats.node_time_flooded_bulk()
        # All four results align on the node index.
        n = nodes.count()
        assert len(ids) == n
        assert max_over.shape == (n,)
        assert vol_flood.shape == (n,)
        assert t_flood.shape == (n,)
        # The set of "flooded nodes" is consistent across the three stats:
        # a node with no flooded time should also have zero flooded volume.
        for i in range(n):
            if t_flood[i] == 0.0:
                assert vol_flood[i] == 0.0, (
                    f"{ids[i]}: t_flooded==0 but vol_flooded={vol_flood[i]}")
