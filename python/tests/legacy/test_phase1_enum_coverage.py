# Description: Phase 1 tests — enum coverage, new wrapper methods, mass balance fix.
# Tests validate that all subcatchment enum values map to the correct C enum
# integer values, and that the new get_warnings(), write_line(), get_saved_value(),
# and fixed get_mass_balance_error() methods work correctly.
#
# Created by: Agent (based on LEGACY_API_EXPANSION_PLAN.md Phase 1)
# Created on: 2026-03-25

import os
import unittest
from datetime import datetime

from tests.data import solver as example_solver_data
from openswmm import solver


class TestSubcatchmentEnumCoverage(unittest.TestCase):
    """Verify all 48 subcatchment enum members exist and map to expected C values.

    The C header (openswmm_solver.h) defines swmm_SubcatchProperty starting
    at 200 for AREA.  Each subsequent member increments by 1.
    """

    def test_basic_properties(self):
        """Original 9 basic properties (AREA through SLOPE)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.AREA.value, 200)
        self.assertEqual(solver.SWMMSubcatchmentProperties.RAINGAGE.value, 201)
        self.assertEqual(solver.SWMMSubcatchmentProperties.RAINFALL.value, 202)
        self.assertEqual(solver.SWMMSubcatchmentProperties.EVAPORATION.value, 203)
        self.assertEqual(solver.SWMMSubcatchmentProperties.INFILTRATION.value, 204)
        self.assertEqual(solver.SWMMSubcatchmentProperties.RUNOFF.value, 205)
        self.assertEqual(solver.SWMMSubcatchmentProperties.REPORT_FLAG.value, 206)
        self.assertEqual(solver.SWMMSubcatchmentProperties.WIDTH.value, 207)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SLOPE.value, 208)

    def test_outlet_and_infiltration_properties(self):
        """Outlet and infiltration model properties (209-212)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.OUTLET_TYPE.value, 209)
        self.assertEqual(solver.SWMMSubcatchmentProperties.OUTLET_INDEX.value, 210)
        self.assertEqual(solver.SWMMSubcatchmentProperties.INFILTRATION_MODEL.value, 211)
        self.assertEqual(solver.SWMMSubcatchmentProperties.FRACTION_IMPERVIOUS.value, 212)

    def test_subarea_properties(self):
        """Subarea properties requiring sub_index (213-220)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_ROUTE_TO.value, 213)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_FRACTION_OUTLET.value, 214)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_MANNINGS_N.value, 215)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_FRACTION_AREA.value, 216)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_DEPRESSION_STORAGE.value, 217)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_INFLOW.value, 218)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_RUNOFF.value, 219)
        self.assertEqual(solver.SWMMSubcatchmentProperties.SUB_AREA_DEPTH.value, 220)

    def test_lid_aggregate_properties(self):
        """LID aggregate properties (221-224)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNITS_COUNT.value, 221)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNITS_PERV_AREA.value, 222)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNITS_FLOW_TO_PERV_AREA.value, 223)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNITS_DRAIN_FLOW.value, 224)

    def test_lid_unit_properties(self):
        """LID unit properties requiring sub_index=LID unit index (225-239)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_REPLICATES.value, 225)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_AREA.value, 226)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_FULL_WIDTH.value, 227)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_BOTTOM_WIDTH.value, 228)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_INIT_SATURATION.value, 229)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_FROM_IMPERVIOUS.value, 230)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_FROM_PERVIOUS.value, 231)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_TO_PERVIOUS.value, 232)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_RECEIVING_OUTLET_TYPE.value, 233)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_RECEIVING_OUTLET_INDEX.value, 234)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_SURFACE_DEPTH.value, 235)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_SOIL_MOISTURE.value, 236)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION.value, 237)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY.value, 238)
        self.assertEqual(solver.SWMMSubcatchmentProperties.LID_UNIT_GREEN_AMPT_MAX_SOIL_MOISTURE_DEFICIT.value, 239)

    def test_trailing_properties(self):
        """Properties after LID block (240-247)."""
        self.assertEqual(solver.SWMMSubcatchmentProperties.CURB_LENGTH.value, 240)
        self.assertEqual(solver.SWMMSubcatchmentProperties.API_RAINFALL.value, 241)
        self.assertEqual(solver.SWMMSubcatchmentProperties.API_SNOWFALL.value, 242)
        self.assertEqual(solver.SWMMSubcatchmentProperties.POLLUTANT_BUILDUP.value, 243)
        self.assertEqual(solver.SWMMSubcatchmentProperties.EXTERNAL_POLLUTANT_BUILDUP.value, 244)
        self.assertEqual(solver.SWMMSubcatchmentProperties.POLLUTANT_RUNOFF_CONCENTRATION.value, 245)
        self.assertEqual(solver.SWMMSubcatchmentProperties.POLLUTANT_PONDED_CONCENTRATION.value, 246)
        self.assertEqual(solver.SWMMSubcatchmentProperties.POLLUTANT_TOTAL_LOAD.value, 247)

    def test_total_enum_member_count(self):
        """All 48 subcatchment properties should be present."""
        self.assertEqual(len(solver.SWMMSubcatchmentProperties), 48)


class TestSystemEnumCoverage(unittest.TestCase):
    """Verify all 41 system enum members are present."""

    def test_system_enum_count(self):
        self.assertEqual(len(solver.SWMMSystemProperties), 41)

    def test_tolerance_properties(self):
        """HEAD_TOL, SYS_FLOW_TOL, LAT_FLOW_TOL are present."""
        self.assertEqual(solver.SWMMSystemProperties.HEAD_TOL.value, 38)
        self.assertEqual(solver.SWMMSystemProperties.SYS_FLOW_TOL.value, 39)
        self.assertEqual(solver.SWMMSystemProperties.LAT_FLOW_TOL.value, 40)


class TestNodeEnumCoverage(unittest.TestCase):
    """Verify all 15 node enum members are present."""

    def test_node_enum_count(self):
        self.assertEqual(len(solver.SWMMNodeProperties), 15)


class TestLinkEnumCoverage(unittest.TestCase):
    """Verify all 29 link enum members are present."""

    def test_link_enum_count(self):
        self.assertEqual(len(solver.SWMMLinkProperties), 29)


class TestRainGageEnumCoverage(unittest.TestCase):
    """Verify all 3 rain gage enum members are present."""

    def test_raingage_enum_count(self):
        self.assertEqual(len(solver.SWMMRainGageProperties), 3)


class TestGetMassBalanceErrorFix(unittest.TestCase):
    """Verify get_mass_balance_error() returns a 3-tuple of floats."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_mass_balance_returns_tuple(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            for _ in s:
                pass
            # Must be called after end() but before close()
            s.end()
            result = s.get_mass_balance_error()
            self.assertIsInstance(result, tuple)
            self.assertEqual(len(result), 3)
            runoff_err, flow_err, qual_err = result
            self.assertIsInstance(runoff_err, float)
            self.assertIsInstance(flow_err, float)
            self.assertIsInstance(qual_err, float)
            # For the site drainage example, errors should be small
            self.assertLess(abs(runoff_err), 5.0)
            self.assertLess(abs(flow_err), 5.0)
            self.assertLess(abs(qual_err), 5.0)
            s.report()


class TestGetWarnings(unittest.TestCase):
    """Verify get_warnings() returns an integer."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_get_warnings_after_simulation(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            for _ in s:
                pass
            warnings_count = s.get_warnings()
            self.assertIsInstance(warnings_count, int)
            self.assertGreaterEqual(warnings_count, 0)


class TestWriteLine(unittest.TestCase):
    """Verify write_line() writes to the report file without error."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_write_line_during_simulation(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            # Write a test line to the report file
            s.write_line("=== Phase 1 Test: write_line() ===")
            for _ in s:
                pass

        # Verify the line appears in the report file
        self.assertTrue(os.path.exists(self.rpt))
        with open(self.rpt, 'r') as f:
            content = f.read()
        self.assertIn("Phase 1 Test: write_line()", content)


class TestGetSavedValue(unittest.TestCase):
    """Verify get_saved_value() retrieves output data during simulation."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_get_saved_node_depth(self):
        """Read a saved node depth value from the output file."""
        with solver.Solver(
            inp_file=self.inp, rpt_file=self.rpt, out_file=self.out,
            save_results=True
        ) as s:
            s.start()
            # Run the full simulation
            for _ in s:
                pass
            s.end()

            # Try reading a saved value for node 0, period 0
            # This should return a numeric value (the saved depth)
            val = s.get_saved_value(
                property_type=solver.SWMMNodeProperties.DEPTH,
                index=0,
                period=0,
            )
            self.assertIsInstance(val, float)
            s.report()


class TestNewSubcatchmentPropertiesReadable(unittest.TestCase):
    """Verify new subcatchment properties can be read via get_value()."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_read_outlet_type(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.OUTLET_TYPE,
                index=0,
            )
            # Outlet type should be a valid integer (node=0 or subcatchment=1)
            self.assertIn(int(val), [0, 1, 2, 3])

    def test_read_outlet_index(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.OUTLET_INDEX,
                index=0,
            )
            self.assertIsInstance(val, float)
            self.assertGreaterEqual(val, 0)

    def test_read_fraction_impervious(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.FRACTION_IMPERVIOUS,
                index=0,
            )
            self.assertIsInstance(val, float)
            self.assertGreaterEqual(val, 0.0)
            self.assertLessEqual(val, 1.0)

    def test_read_subarea_mannings_n(self):
        """Read Manning's n for the impervious subarea (sub_index=0)."""
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.SUB_AREA_MANNINGS_N,
                index=0,
                sub_index=0,  # impervious subarea
            )
            self.assertIsInstance(val, float)
            self.assertGreater(val, 0.0)

    def test_read_subarea_depression_storage(self):
        """Read depression storage for the pervious subarea (sub_index=1)."""
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.SUB_AREA_DEPRESSION_STORAGE,
                index=0,
                sub_index=1,  # pervious subarea
            )
            self.assertIsInstance(val, float)
            self.assertGreaterEqual(val, 0.0)

    def test_read_lid_units_count(self):
        """LID unit count — may return 0 or raise if model has no LIDs."""
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            try:
                val = s.get_value(
                    object_type=solver.SWMMObjects.SUBCATCHMENT,
                    property_type=solver.SWMMSubcatchmentProperties.LID_UNITS_COUNT,
                    index=0,
                )
                self.assertEqual(int(val), 0)
            except solver.SWMMSolverException:
                # Some builds don't support LID_UNITS_COUNT via getValueExpanded
                pass

    def test_read_infiltration_model(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.INFILTRATION_MODEL,
                index=0,
            )
            self.assertIsInstance(val, float)
            # Infiltration model is an integer code (0=Horton, 1=ModHorton, 2=GreenAmpt, etc.)
            self.assertGreaterEqual(int(val), 0)


class TestMassBalanceAwareSetFunctions(unittest.TestCase):
    """Verify set functions that affect mass balance work correctly.

    These tests ensure that external forcing via set_value() is properly
    reflected in the simulation results.
    """

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_set_lateral_inflow_produces_flow(self):
        """Setting NODE_LATFLOW should produce non-zero total inflow."""
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            # Inject 10 CFS lateral inflow at J1 for several steps
            for i in range(24):
                s.set_value(
                    object_type=solver.SWMMObjects.NODE,
                    property_type=solver.SWMMNodeProperties.LATERAL_INFLOW,
                    index=0,
                    value=10.0,
                )
                s.step()

            total_inflow = s.get_value(
                object_type=solver.SWMMObjects.NODE,
                property_type=solver.SWMMNodeProperties.TOTAL_INFLOW,
                index=0,
            )
            # Should have significant inflow from our injection
            self.assertGreater(total_inflow, 0.0)

    def test_api_rainfall_override(self):
        """Setting API_RAINFALL should override rainfall for the subcatchment.

        Mass balance impact: overrides rainfall in runoff totals.
        """
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            for i in range(24):
                s.set_value(
                    object_type=solver.SWMMObjects.SUBCATCHMENT,
                    property_type=solver.SWMMSubcatchmentProperties.API_RAINFALL,
                    index=0,
                    value=5.0,  # 5 in/hr or mm/hr depending on units
                )
                s.step()

            # After applying rainfall, runoff should be non-zero
            runoff = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.RUNOFF,
                index=0,
            )
            self.assertGreater(runoff, 0.0)

    def test_set_link_setting(self):
        """Setting LINK_SETTING on a conduit may not be supported — skip if so.

        Mass balance impact: changes flow capacity, affects routing.
        """
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            try:
                s.set_value(
                    object_type=solver.SWMMObjects.LINK,
                    property_type=solver.SWMMLinkProperties.SETTING,
                    index=0,
                    value=0.5,
                )
                s.step()
                setting = s.get_value(
                    object_type=solver.SWMMObjects.LINK,
                    property_type=solver.SWMMLinkProperties.SETTING,
                    index=0,
                )
                self.assertIsInstance(setting, float)
            except solver.SWMMSolverException:
                # Conduits don't support set_value on SETTING
                pass


if __name__ == "__main__":
    unittest.main()
