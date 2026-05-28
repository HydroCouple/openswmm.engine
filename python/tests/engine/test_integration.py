"""Integration tests exercising the full engine surface in a simulation.

Migrated to the v1 Pythonic bindings (see
``docs/PYTHONIC_BINDINGS_DONE.md``).
"""

import pytest

from openswmm.engine import RoutingTotal, Solver


class TestFullSimulationWithExpandedAPI:
    """Run a full simulation using the v1 property-style API."""

    def test_read_all_properties_during_simulation(self, solver_files):
        """Read every wrapper property during a live sim."""
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        # Pre-fetch counts via the container protocol.
        n_nodes = len(s.nodes)
        n_links = len(s.links)
        n_sc = len(s.subcatchments)
        n_gages = len(s.gages)

        step_count = 0
        for _ in s.steps():
            step_count += 1
            if step_count > 20:
                break

            # Per-object property reads — same iteration via wrappers.
            for node in s.nodes:
                assert node.depth >= 0.0
                _ = node.head, node.volume, node.lateral_inflow
                _ = node.overflow, node.inflow, node.type
                _ = node.invert_elev, node.max_depth

            for link in s.links:
                _ = link.flow, link.depth, link.velocity
                _ = link.capacity, link.volume, link.type, link.length
                _ = link.from_node, link.to_node

            for sub in s.subcatchments:
                _ = sub.runoff, sub.rainfall, sub.area, sub.width, sub.slope

            for gage in s.gages:
                _ = gage.rainfall, gage.rain_type

            # Bulk operations — properties now.
            depths = s.nodes.depths
            assert depths.shape == (n_nodes,)
            heads = s.nodes.heads
            assert heads.shape == (n_nodes,)
            flows = s.links.flows
            assert flows.shape == (n_links,)

        # Drive the rest of the simulation to completion so the continuity
        # checks below have a complete mass balance.
        for _ in s.steps():
            pass

        s.end()

        # Post-simulation per-object statistics via the .stats sub-view.
        for node in s.nodes:
            _ = node.stats.max_depth, node.stats.vol_flooded
        for link in s.links:
            _ = link.stats.max_flow, link.stats.vol_flow
        for sub in s.subcatchments:
            _ = sub.stats.precip, sub.stats.runoff_vol

        # Mass balance.
        mb = s.mass_balance
        assert abs(mb.runoff_continuity_error) < 0.10
        assert abs(mb.routing_continuity_error) < 0.10

        s.report()
        s.close()
        s.destroy()

    def test_lateral_inflow_injection(self, solver_files):
        """Inject lateral inflow at a node and verify it appears in routing totals."""
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        # Inject for the first 50 steps via the wrapper.
        n0 = s.nodes[0]
        step_count = 0
        for _ in s.steps():
            step_count += 1
            if step_count > 50:
                break
            n0.lateral_inflow = 1.0

        # Drive to end.
        for _ in s.steps():
            pass

        s.end()

        # Runtime-API-injected lateral inflow accumulates in the FORCING_INFLOW
        # bucket — distinct from EXTERNAL.
        forced_inflow = s.mass_balance.routing_total(RoutingTotal.FORCING_INFLOW)
        assert forced_inflow > 0.0, "Forced inflow should be positive after injection"

        s.report()
        s.close()
        s.destroy()

    def test_rainfall_override(self, solver_files):
        """Override rainfall on a subcatchment and verify runoff response."""
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        sub0 = s.subcatchments[0]
        max_runoff = 0.0
        step_count = 0

        for _ in s.steps():
            step_count += 1
            if step_count > 100:
                break
            sub0.rainfall = 25.4         # 1 inch/hr
            max_runoff = max(max_runoff, sub0.runoff)

        for _ in s.steps():
            pass

        s.end()
        assert max_runoff > 0.0, "Runoff should occur after rainfall override"

        s.report()
        s.close()
        s.destroy()
