"""Integration tests exercising the full expanded engine API in a simulation."""

import numpy as np
import pytest

from openswmm.engine import Solver, Nodes, Links, Subcatchments, Gages, MassBalance, EngineState


class TestFullSimulationWithExpandedAPI:
    """Run a full simulation using all expanded accessor methods."""

    def test_read_all_properties_during_simulation(self, solver_files):
        """Read every expanded property from every object during a live sim."""
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        nodes = Nodes(s)
        links = Links(s)
        sc = Subcatchments(s)
        gages = Gages(s)

        n_nodes = nodes.count()
        n_links = links.count()
        n_sc = sc.count()
        n_gages = gages.count()

        step_count = 0
        # Read properties through the FIRST 20 timesteps, then continue
        # silently to completion so continuity stats are meaningful.
        while s.state == EngineState.RUNNING and step_count < 20:
            if s.step() != 0:
                break
            step_count += 1

            # Node properties
            for i in range(n_nodes):
                assert nodes.get_depth(i) >= 0.0
                nodes.get_head(i)
                nodes.get_volume(i)
                nodes.get_lateral_inflow(i)
                nodes.get_overflow(i)
                nodes.get_inflow(i)
                nodes.get_type(i)
                nodes.get_invert_elev(i)
                nodes.get_max_depth(i)

            # Link properties
            for i in range(n_links):
                links.get_flow(i)
                links.get_depth(i)
                links.get_velocity(i)
                links.get_capacity(i)
                links.get_volume(i)
                links.get_type(i)
                links.get_length(i)
                links.get_from_node(i)
                links.get_to_node(i)

            # Subcatchment properties
            for i in range(n_sc):
                sc.get_runoff(i)
                sc.get_rainfall(i)
                sc.get_area(i)
                sc.get_width(i)
                sc.get_slope(i)

            # Gage properties
            for i in range(n_gages):
                gages.get_rainfall(i)
                gages.get_rain_type(i)

            # Bulk operations
            depths = nodes.get_depths_bulk()
            assert depths.shape == (n_nodes,)
            heads = nodes.get_heads_bulk()
            assert heads.shape == (n_nodes,)
            flows = links.get_flows_bulk()
            assert flows.shape == (n_links,)

        # Drive the rest of the simulation to completion so the continuity
        # checks below have a complete mass balance.
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break

        s.end()

        # Post-simulation statistics
        for i in range(n_nodes):
            nodes.get_stat_max_depth(i)
            nodes.get_stat_vol_flooded(i)
        for i in range(n_links):
            links.get_stat_max_flow(i)
            links.get_stat_vol_flow(i)
        for i in range(n_sc):
            sc.get_stat_precip(i)
            sc.get_stat_runoff_vol(i)

        # Mass balance
        mb = MassBalance(s)
        runoff_err = mb.get_runoff_continuity_error()
        routing_err = mb.get_routing_continuity_error()
        assert abs(runoff_err) < 0.10  # < 10%
        assert abs(routing_err) < 0.10

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

        nodes = Nodes(s)

        for _ in range(50):
            nodes.set_lateral_inflow(0, 1.0)
            s.step()

        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass

        s.end()

        # Check routing total for external inflow
        mb = MassBalance(s)
        ext_inflow = mb.get_routing_total(4)  # EXTERNAL
        assert ext_inflow > 0.0, "External inflow should be positive after injection"

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

        sc = Subcatchments(s)
        max_runoff = 0.0

        for _ in range(100):
            sc.set_rainfall(0, 25.4)  # 1 inch/hr
            s.step()
            r = sc.get_runoff(0)
            max_runoff = max(max_runoff, r)

        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass

        s.end()
        assert max_runoff > 0.0, "Runoff should occur after rainfall override"

        s.report()
        s.close()
        s.destroy()
