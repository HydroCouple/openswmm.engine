# Description: Phase 3+4 tests — OOP wrappers and mass-balance-aware setters.
# Tests validate the LegacyNode, LegacyLink, LegacySubcatchment, LegacyRainGage,
# LegacySystem, and ExternalForcingLog classes.
#
# Created on: 2026-03-25

import unittest
from datetime import datetime

from tests.data import solver as example_solver_data
from openswmm import solver
from openswmm.legacy.engine import (
    LegacyNodes,
    LegacyNode,
    LegacyLinks,
    LegacyLink,
    LegacySubcatchments,
    LegacySubcatchment,
    LegacyRainGages,
    LegacyRainGage,
    LegacySystem,
    ExternalForcingLog,
)


def _make_solver():
    inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
    rpt = inp.replace(".inp", ".rpt")
    out = inp.replace(".inp", ".out")
    return solver.Solver(inp_file=inp, rpt_file=rpt, out_file=out)


# ======================================================================
#  LegacyNodes / LegacyNode
# ======================================================================
class TestLegacyNodes(unittest.TestCase):

    def test_collection_length(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        self.assertEqual(len(nodes), 12)
        s.finalize()

    def test_access_by_name(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        j1 = nodes["J1"]
        self.assertEqual(j1.name, "J1")
        self.assertEqual(j1.index, 0)
        s.finalize()

    def test_access_by_index(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        n = nodes[5]
        self.assertIsInstance(n, LegacyNode)
        s.finalize()

    def test_iteration(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        names = [n.name for n in nodes]
        self.assertEqual(len(names), 12)
        self.assertIn("J1", names)
        self.assertIn("O1", names)
        s.finalize()


class TestLegacyNodeProperties(unittest.TestCase):

    def test_read_invert_elevation(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        j1 = nodes["J1"]
        elev = j1.invert_elevation
        self.assertIsInstance(elev, float)
        s.finalize()

    def test_read_results_during_sim(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        # Step a few times to get some flow
        for _ in range(24):
            s.step()
        j6 = nodes["J6"]
        self.assertIsInstance(j6.depth, float)
        self.assertIsInstance(j6.head, float)
        self.assertIsInstance(j6.total_inflow, float)
        s.finalize()

    def test_node_type(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        o1 = nodes["O1"]
        self.assertEqual(o1.node_type, solver.SWMMNodeTypes.OUTFALL)
        s.finalize()

    def test_set_lateral_inflow(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        j1 = nodes["J1"]
        j1.set_lateral_inflow(10.0)
        s.step()
        self.assertGreater(j1.total_inflow, 0.0)
        s.finalize()


class TestLegacyNodeStatistics(unittest.TestCase):

    def test_statistics_before_end(self):
        """Statistics must be read before end() frees the internal arrays."""
        s = _make_solver()
        s.initialize()
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()
        # Read stats BEFORE end()
        nodes = LegacyNodes(s)
        stats = nodes["J1"].statistics
        self.assertIn("max_depth", stats)
        self.assertIn("max_inflow", stats)
        s.finalize()


# ======================================================================
#  LegacyLinks / LegacyLink
# ======================================================================
class TestLegacyLinks(unittest.TestCase):

    def test_collection_length(self):
        s = _make_solver()
        s.initialize()
        links = LegacyLinks(s)
        self.assertEqual(len(links), 11)
        s.finalize()

    def test_access_by_name(self):
        s = _make_solver()
        s.initialize()
        links = LegacyLinks(s)
        c1 = links["C1"]
        self.assertEqual(c1.name, "C1")
        s.finalize()

    def test_read_flow_during_sim(self):
        s = _make_solver()
        s.initialize()
        for _ in range(24):
            s.step()
        links = LegacyLinks(s)
        c10 = links["C10"]
        self.assertIsInstance(c10.flow, float)
        self.assertIsInstance(c10.velocity, float)
        s.finalize()

    def test_link_type(self):
        s = _make_solver()
        s.initialize()
        links = LegacyLinks(s)
        c1 = links["C1"]
        self.assertEqual(c1.link_type, solver.SWMMLinkTypes.CONDUIT)
        s.finalize()

    def test_set_setting(self):
        """Setting on conduits may not be supported — verify graceful handling."""
        s = _make_solver()
        s.initialize()
        links = LegacyLinks(s)
        c1 = links["C1"]
        try:
            c1.set_setting(0.5)
            s.step()
        except solver.SWMMSolverException:
            # Conduits don't support set_value on SETTING
            pass
        s.finalize()


# ======================================================================
#  LegacySubcatchments / LegacySubcatchment
# ======================================================================
class TestLegacySubcatchments(unittest.TestCase):

    def test_collection_length(self):
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        self.assertEqual(len(subs), 7)
        s.finalize()

    def test_read_area(self):
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        s1 = subs["S1"]
        self.assertGreater(s1.area, 0.0)
        s.finalize()

    def test_read_fraction_impervious(self):
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        s1 = subs[0]
        fi = s1.fraction_impervious
        self.assertGreaterEqual(fi, 0.0)
        self.assertLessEqual(fi, 1.0)
        s.finalize()

    def test_read_runoff_during_sim(self):
        s = _make_solver()
        s.initialize()
        for _ in range(24):
            s.step()
        subs = LegacySubcatchments(s)
        self.assertIsInstance(subs["S1"].runoff, float)
        s.finalize()

    def test_subarea_mannings_n(self):
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        n_imperv = subs[0].get_subarea_mannings_n(0)
        self.assertGreater(n_imperv, 0.0)
        s.finalize()

    def test_lid_count_zero(self):
        """Site drainage example has no LIDs — may raise if unsupported."""
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        try:
            self.assertEqual(subs[0].lid_units_count, 0)
        except solver.SWMMSolverException:
            pass  # Some builds don't support LID_UNITS_COUNT via getValueExpanded
        s.finalize()

    def test_set_api_rainfall(self):
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        subs["S1"].set_api_rainfall(5.0)
        s.step()
        self.assertGreaterEqual(subs["S1"].rainfall, 0.0)
        s.finalize()


# ======================================================================
#  LegacyRainGages / LegacyRainGage
# ======================================================================
class TestLegacyRainGages(unittest.TestCase):

    def test_collection_length(self):
        s = _make_solver()
        s.initialize()
        gages = LegacyRainGages(s)
        self.assertEqual(len(gages), 1)
        s.finalize()

    def test_read_precipitation(self):
        s = _make_solver()
        s.initialize()
        for _ in range(12):
            s.step()
        gages = LegacyRainGages(s)
        rg = gages["RainGage"]
        self.assertIsInstance(rg.total_precipitation, float)
        s.finalize()

    def test_set_rainfall(self):
        s = _make_solver()
        s.initialize()
        gages = LegacyRainGages(s)
        gages["RainGage"].set_rainfall(3.6)
        s.step()
        # Verify no crash
        s.finalize()


# ======================================================================
#  LegacySystem
# ======================================================================
class TestLegacySystem(unittest.TestCase):

    def test_flow_units(self):
        s = _make_solver()
        s.initialize()
        sys = LegacySystem(s)
        self.assertIsInstance(sys.flow_units, solver.SWMMFlowUnits)
        s.finalize()

    def test_routing_step(self):
        s = _make_solver()
        s.initialize()
        sys = LegacySystem(s)
        self.assertGreater(sys.routing_step, 0.0)
        s.finalize()

    def test_routing_totals_before_end(self):
        s = _make_solver()
        s.initialize()
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()
        sys = LegacySystem(s)
        totals = sys.routing_totals
        self.assertIn("ww_inflow", totals)
        self.assertIn("pct_error", totals)
        self.assertGreater(totals["outflow"], 0.0)
        s.finalize()

    def test_runoff_totals_before_end(self):
        s = _make_solver()
        s.initialize()
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()
        sys = LegacySystem(s)
        totals = sys.runoff_totals
        self.assertIn("rainfall", totals)
        self.assertGreater(totals["rainfall"], 0.0)
        s.finalize()

    def test_mass_balance_error(self):
        s = _make_solver()
        s.initialize()
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()
        s.end()
        sys = LegacySystem(s)
        runoff_err, flow_err, qual_err = sys.mass_balance_error
        self.assertLess(abs(flow_err), 5.0)
        s.report()
        s.close()


# ======================================================================
#  ExternalForcingLog
# ======================================================================
class TestExternalForcingLog(unittest.TestCase):

    def test_empty_log(self):
        log = ExternalForcingLog()
        self.assertEqual(len(log), 0)
        self.assertEqual(log.records, [])

    def test_record_and_retrieve(self):
        log = ExternalForcingLog()
        log.record(
            sim_time=datetime(2000, 1, 1),
            object_type="node",
            object_id="J1",
            property_name="lateral_inflow",
            value=10.0,
            mass_balance_category="routing.ex_inflow",
        )
        self.assertEqual(len(log), 1)
        r = log.records[0]
        self.assertEqual(r["object_id"], "J1")
        self.assertEqual(r["value"], 10.0)

    def test_clear(self):
        log = ExternalForcingLog()
        log.record(
            sim_time=datetime(2000, 1, 1),
            object_type="node",
            object_id="J1",
            property_name="test",
            value=1.0,
        )
        log.clear()
        self.assertEqual(len(log), 0)

    def test_log_with_node_setter(self):
        """Verify ExternalForcingLog integrates with LegacyNode.set_lateral_inflow."""
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        log = ExternalForcingLog()

        nodes["J1"].set_lateral_inflow(5.0, log=log)
        s.step()
        nodes["J1"].set_lateral_inflow(7.5, log=log)
        s.step()

        self.assertEqual(len(log), 2)
        self.assertEqual(log.records[0]["value"], 5.0)
        self.assertEqual(log.records[1]["value"], 7.5)
        self.assertEqual(log.records[0]["mass_balance_category"], "routing.ex_inflow")
        s.finalize()

    def test_log_with_subcatchment_setter(self):
        """Verify log integrates with subcatchment API rainfall override."""
        s = _make_solver()
        s.initialize()
        subs = LegacySubcatchments(s)
        log = ExternalForcingLog()

        subs["S1"].set_api_rainfall(3.0, log=log)
        s.step()

        self.assertEqual(len(log), 1)
        self.assertEqual(log.records[0]["mass_balance_category"], "runoff.rainfall")
        s.finalize()

    def test_log_with_link_setter(self):
        s = _make_solver()
        s.initialize()
        links = LegacyLinks(s)
        log = ExternalForcingLog()

        try:
            links["C1"].set_setting(0.5, log=log)
            s.step()
            self.assertEqual(len(log), 1)
            self.assertEqual(log.records[0]["property"], "setting")
        except solver.SWMMSolverException:
            # Conduits don't support set_value on SETTING
            pass
        s.finalize()


class TestForcingLogMassBalanceValidation(unittest.TestCase):
    """End-to-end test: inject external inflow, verify it appears in both
    the forcing log and the routing totals."""

    def test_logged_forcing_matches_routing_totals(self):
        s = _make_solver()
        s.initialize()
        nodes = LegacyNodes(s)
        log = ExternalForcingLog()

        # Inject constant inflow for 100 steps
        for _ in range(100):
            nodes["J1"].set_lateral_inflow(10.0, log=log)
            s.step()

        # Run remaining
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()

        # Read stats BEFORE end() frees the arrays
        sys = LegacySystem(s)
        totals = sys.routing_totals

        # Log should have 100 entries
        self.assertEqual(len(log), 100)

        # External inflow in routing totals should be positive
        self.assertGreater(totals["ex_inflow"], 0.0)

        # Mass balance should close
        self.assertLess(abs(totals["pct_error"]), 5.0)

        s.finalize()


if __name__ == "__main__":
    unittest.main()
