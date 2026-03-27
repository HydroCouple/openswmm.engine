# Description: Legacy unit tests for the openswmmcore output module.
# Originally: tests/test_swwm_output.py
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19
# Refactored: 2026-03-25

import pickle
import unittest
from datetime import datetime

from tests.data import output as example_output_data
from openswmm import output
from openswmm.legacy.output import Output, SWMMOutputException


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
def assert_dict_almost_equal(d1: dict, d2: dict, rtol=1e-5, atol=1e-8):
    """Recursively compare two dicts with floating-point tolerance."""
    assert set(d1.keys()) == set(d2.keys()), f"Key mismatch: {set(d1) ^ set(d2)}"
    for key in d1:
        v1, v2 = d1[key], d2[key]
        if isinstance(v1, dict):
            assert_dict_almost_equal(v1, v2, rtol, atol)
        elif isinstance(v1, float):
            assert abs(v1 - v2) <= atol + rtol * abs(v2), (
                f"Key {key!r}: {v1} != {v2} (tol={atol}+{rtol}*|{v2}|)"
            )
        else:
            assert v1 == v2, f"Key {key!r}: {v1!r} != {v2!r}"


# ---------------------------------------------------------------------------
# Enum tests
# ---------------------------------------------------------------------------
class TestOutputEnums(unittest.TestCase):
    """Verify that output enum values match the C API definitions."""

    def test_unit_system(self):
        self.assertEqual(output.UnitSystem.US.value, 0)
        self.assertEqual(output.UnitSystem.SI.value, 1)

    def test_flow_units(self):
        expected = {"CFS": 0, "GPM": 1, "MGD": 2, "CMS": 3, "LPS": 4, "MLD": 5}
        for name, val in expected.items():
            self.assertEqual(output.FlowUnits[name].value, val)

    def test_concentration_units(self):
        expected = {"MG": 0, "UG": 1, "COUNT": 2, "NONE": 3}
        for name, val in expected.items():
            self.assertEqual(output.ConcentrationUnits[name].value, val)

    def test_element_type(self):
        expected = {"SUBCATCHMENT": 0, "NODE": 1, "LINK": 2, "SYSTEM": 3, "POLLUTANT": 4}
        for name, val in expected.items():
            self.assertEqual(output.ElementType[name].value, val)

    def test_time_attribute(self):
        self.assertEqual(output.TimeAttribute.REPORT_STEP.value, 0)
        self.assertEqual(output.TimeAttribute.NUM_PERIODS.value, 1)

    def test_subcatch_attribute(self):
        expected = [
            "RAINFALL", "SNOW_DEPTH", "EVAPORATION_LOSS", "INFILTRATION_LOSS",
            "RUNOFF_RATE", "GROUNDWATER_OUTFLOW", "GROUNDWATER_TABLE_ELEVATION",
            "SOIL_MOISTURE", "POLLUTANT_CONCENTRATION",
        ]
        for i, name in enumerate(expected):
            self.assertEqual(output.SubcatchAttribute[name].value, i)

    def test_node_attribute(self):
        expected = [
            "INVERT_DEPTH", "HYDRAULIC_HEAD", "STORED_VOLUME",
            "LATERAL_INFLOW", "TOTAL_INFLOW", "FLOODING_LOSSES",
            "POLLUTANT_CONCENTRATION",
        ]
        for i, name in enumerate(expected):
            self.assertEqual(output.NodeAttribute[name].value, i)

    def test_link_attribute(self):
        expected = [
            "FLOW_RATE", "FLOW_DEPTH", "FLOW_VELOCITY",
            "FLOW_VOLUME", "CAPACITY", "POLLUTANT_CONCENTRATION",
        ]
        for i, name in enumerate(expected):
            self.assertEqual(output.LinkAttribute[name].value, i)

    def test_system_attribute(self):
        expected = [
            "AIR_TEMP", "RAINFALL", "SNOW_DEPTH", "EVAP_INFIL_LOSS",
            "RUNOFF_FLOW", "DRY_WEATHER_INFLOW", "GROUNDWATER_INFLOW",
            "RDII_INFLOW", "DIRECT_INFLOW", "TOTAL_LATERAL_INFLOW",
            "FLOOD_LOSSES", "OUTFALL_FLOWS", "VOLUME_STORED", "EVAPORATION_RATE",
        ]
        for i, name in enumerate(expected):
            self.assertEqual(output.SystemAttribute[name].value, i)


# ---------------------------------------------------------------------------
# File I/O tests
# ---------------------------------------------------------------------------
class TestOutputFileIO(unittest.TestCase):
    """Tests for opening/closing output files and basic metadata."""

    def test_open_and_close_context_manager(self):
        with Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1):
            pass

    def test_open_and_close_manual(self):
        Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)

    def test_open_nonexistent_raises(self):
        with self.assertRaises(FileNotFoundError) as ctx:
            Output(output_file=example_output_data.NON_EXISTENT_OUTPUT_FILE)
        self.assertIn("Error opening the SWMM output file", str(ctx.exception))

    def test_version(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        self.assertEqual(out.version, 51000)

    def test_output_size(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        self.assertDictEqual(
            out.output_size,
            {"subcatchments": 8, "nodes": 14, "links": 13, "system": 1, "pollutants": 2},
        )

    def test_units(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        units = list(out.units)
        self.assertEqual(units[0], output.UnitSystem.US)
        self.assertEqual(units[1], output.FlowUnits.CFS)
        self.assertListEqual(
            units[2], [output.ConcentrationUnits.MG, output.ConcentrationUnits.UG]
        )

    def test_flow_units(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        self.assertEqual(out.flow_units, output.FlowUnits.CFS)

    def test_start_date(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        self.assertEqual(out.start_date, datetime(1998, 1, 1))

    def test_time_attributes(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        self.assertEqual(out.get_time_attribute(output.TimeAttribute.REPORT_STEP), 3600)
        self.assertEqual(out.get_time_attribute(output.TimeAttribute.NUM_PERIODS), 36)

    def test_times(self):
        out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        times = out.times
        self.assertEqual(len(times), 36)
        self.assertEqual(times[0], datetime(1998, 1, 1, 1))
        self.assertEqual(times[16], datetime(1998, 1, 1, 17))
        self.assertEqual(times[-1], datetime(1998, 1, 2, 12))


# ---------------------------------------------------------------------------
# Element property tests
# ---------------------------------------------------------------------------
class TestOutputElementProperties(unittest.TestCase):
    """Tests for element property queries and variable counts."""

    def setUp(self):
        self.out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)

    def test_num_properties(self):
        self.assertEqual(self.out.get_num_properties(output.ElementType.SUBCATCHMENT), 1)
        self.assertEqual(self.out.get_num_properties(output.ElementType.NODE), 3)
        self.assertEqual(self.out.get_num_properties(output.ElementType.LINK), 5)

    def test_property_codes(self):
        self.assertListEqual(
            self.out.get_property_codes(output.ElementType.SUBCATCHMENT), [1]
        )
        self.assertListEqual(
            self.out.get_property_codes(output.ElementType.NODE), [0, 2, 3]
        )
        self.assertListEqual(
            self.out.get_property_codes(output.ElementType.LINK), [0, 4, 4, 3, 5]
        )

    def test_property_values_subcatchment(self):
        vals = self.out.get_property_values(output.ElementType.SUBCATCHMENT, 0)
        self.assertListEqual(vals, [10.0])

    def test_property_values_node(self):
        vals = self.out.get_property_values(output.ElementType.NODE, 0)
        self.assertListEqual(vals, [0.0, 1000.0, 3.0])

    def test_property_values_link(self):
        vals = self.out.get_property_values(output.ElementType.LINK, 0)
        self.assertListEqual(vals, [0.0, 0.0, 0.0, 1.5, 400.0])

    def test_num_variables(self):
        self.assertEqual(self.out.get_num_variables(output.ElementType.SUBCATCHMENT), 10)
        self.assertEqual(self.out.get_num_variables(output.ElementType.NODE), 8)
        self.assertEqual(self.out.get_num_variables(output.ElementType.LINK), 7)
        self.assertEqual(self.out.get_num_variables(output.ElementType.SYSTEM), 14)


# ---------------------------------------------------------------------------
# Element name tests
# ---------------------------------------------------------------------------
class TestOutputElementNames(unittest.TestCase):
    """Tests for element name retrieval."""

    def setUp(self):
        self.out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)

    def test_subcatchment_names(self):
        names = [self.out.get_element_name(output.ElementType.SUBCATCHMENT, i) for i in range(8)]
        self.assertListEqual(names, ["1", "2", "3", "4", "5", "6", "7", "8"])

    def test_node_names(self):
        names = [self.out.get_element_name(output.ElementType.NODE, i) for i in range(14)]
        expected = ["9", "10", "13", "14", "15", "16", "17", "19", "20", "21", "22", "23", "24", "18"]
        self.assertListEqual(names, expected)

    def test_link_names(self):
        names = [self.out.get_element_name(output.ElementType.LINK, i) for i in range(13)]
        expected = ["1", "4", "5", "6", "7", "8", "10", "11", "12", "13", "14", "15", "16"]
        self.assertListEqual(names, expected)

    def test_pollutant_names(self):
        names = [self.out.get_element_name(output.ElementType.POLLUTANT, i) for i in range(2)]
        self.assertListEqual(names, ["TSS", "Lead"])

    def test_bulk_names(self):
        self.assertListEqual(
            self.out.get_element_names(output.ElementType.SUBCATCHMENT),
            ["1", "2", "3", "4", "5", "6", "7", "8"],
        )

    def test_system_name_raises(self):
        with self.assertRaises(Exception):
            self.out.get_element_name(output.ElementType.SYSTEM, 0)

    def test_out_of_range_raises(self):
        with self.assertRaises(Exception):
            self.out.get_element_name(output.ElementType.SUBCATCHMENT, 8)

    def test_bulk_system_raises(self):
        with self.assertRaises(SWMMOutputException):
            self.out.get_element_names(output.ElementType.SYSTEM)


# ---------------------------------------------------------------------------
# Time-series tests
# ---------------------------------------------------------------------------
class TestOutputTimeseries(unittest.TestCase):
    """Tests for time-series data retrieval."""

    def setUp(self):
        self.out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        with open(example_output_data.JSON_TIME_SERIES_FILE, "rb") as f:
            self.artifacts = pickle.load(f)

    def test_subcatchment_timeseries(self):
        ts = self.out.get_subcatchment_timeseries(5, output.SubcatchAttribute.RUNOFF_RATE)
        assert_dict_almost_equal(ts, self.artifacts["test_get_subcatchment_timeseries"])

    def test_subcatchment_timeseries_by_name(self):
        by_idx = self.out.get_subcatchment_timeseries(5, output.SubcatchAttribute.RUNOFF_RATE)
        by_name = self.out.get_subcatchment_timeseries("6", output.SubcatchAttribute.RUNOFF_RATE)
        assert_dict_almost_equal(by_idx, by_name)

    def test_node_timeseries(self):
        ts = self.out.get_node_timeseries(7, output.NodeAttribute.TOTAL_INFLOW)
        assert_dict_almost_equal(ts, self.artifacts["test_get_node_timeseries"])

    def test_node_timeseries_by_name(self):
        by_idx = self.out.get_node_timeseries(7, output.NodeAttribute.TOTAL_INFLOW)
        by_name = self.out.get_node_timeseries("19", output.NodeAttribute.TOTAL_INFLOW)
        assert_dict_almost_equal(by_idx, by_name)

    def test_link_timeseries(self):
        ts = self.out.get_link_timeseries(5, output.LinkAttribute.FLOW_RATE)
        assert_dict_almost_equal(ts, self.artifacts["test_get_link_timeseries"])

    def test_link_timeseries_by_name(self):
        by_idx = self.out.get_link_timeseries(5, output.LinkAttribute.FLOW_RATE)
        by_name = self.out.get_link_timeseries("8", output.LinkAttribute.FLOW_RATE)
        assert_dict_almost_equal(by_idx, by_name)

    def test_system_timeseries(self):
        ts = self.out.get_system_timeseries(output.SystemAttribute.RUNOFF_FLOW)
        assert_dict_almost_equal(ts, self.artifacts["test_get_system_timeseries"])


# ---------------------------------------------------------------------------
# Spatial snapshot tests (values by time + attribute or element)
# ---------------------------------------------------------------------------
class TestOutputSpatialSnapshots(unittest.TestCase):
    """Tests for spatial snapshot retrieval (all elements at a given time)."""

    def setUp(self):
        self.out = Output(output_file=example_output_data.EXAMPLE_OUTPUT_FILE_1)
        with open(example_output_data.JSON_TIME_SERIES_FILE, "rb") as f:
            self.artifacts = pickle.load(f)

    def test_subcatchment_by_time_and_attribute(self):
        vals = self.out.get_subcatchment_values_by_time_and_attribute(5, output.SubcatchAttribute.RUNOFF_RATE)
        assert_dict_almost_equal(vals, self.artifacts["test_get_subcatchment_values_by_time_and_attributes"])

    def test_node_by_time_and_attribute(self):
        vals = self.out.get_node_values_by_time_and_attribute(8, output.NodeAttribute.TOTAL_INFLOW)
        assert_dict_almost_equal(vals, self.artifacts["test_get_node_values_by_time_and_attributes"])

    def test_link_by_time_and_attribute(self):
        vals = self.out.get_link_values_by_time_and_attribute(10, output.LinkAttribute.FLOW_RATE)
        assert_dict_almost_equal(vals, self.artifacts["test_get_link_values_by_time_and_attributes"])

    def test_system_by_time_and_attribute(self):
        vals = self.out.get_system_values_by_time_and_attribute(12, output.SystemAttribute.RUNOFF_FLOW)
        assert_dict_almost_equal(vals, self.artifacts["test_get_system_values_by_time_and_attributes"])

    def test_subcatchment_by_time_and_index(self):
        vals = self.out.get_subcatchment_values_by_time_and_element_index(5, 3)
        assert_dict_almost_equal(vals, self.artifacts["test_get_subcatchment_values_by_time_and_index"])

    def test_subcatchment_by_time_and_name(self):
        by_idx = self.out.get_subcatchment_values_by_time_and_element_index(5, 3)
        by_name = self.out.get_subcatchment_values_by_time_and_element_index(5, "4")
        assert_dict_almost_equal(by_idx, by_name)

    def test_node_by_time_and_index(self):
        vals = self.out.get_node_values_by_time_and_element_index(8, 4)
        assert_dict_almost_equal(vals, self.artifacts["test_get_node_values_by_time_and_index"])

    def test_link_by_time_and_index(self):
        vals = self.out.get_link_values_by_time_and_element_index(10, 5)
        assert_dict_almost_equal(vals, self.artifacts["test_get_link_values_by_time_and_index"])

    def test_system_values_by_time(self):
        vals = self.out.get_system_values_by_time(12)
        assert_dict_almost_equal(vals, self.artifacts["test_get_system_values_by_time"])
