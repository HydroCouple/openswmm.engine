# Description: Legacy unit tests for the openswmmcore solver module.
# Originally: tests/test_swmm_solver.py
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19
# Refactored: 2026-03-25

import os
import unittest
from datetime import datetime

from tests.data import solver as example_solver_data
from openswmm import solver


class TestSolverVersion(unittest.TestCase):
    """Tests for version and date-encoding utilities."""

    def test_get_swmm_version(self):
        version = solver.version()
        self.assertEqual(version, 53000)

    def test_swmm_encode_date(self):
        dt = datetime(year=2024, month=11, day=16, hour=13, minute=33, second=21)
        encoded = solver.encode_swmm_datetime(dt=dt)
        self.assertAlmostEqual(encoded, 45612.564826389)

    def test_swmm_decode_date(self):
        decoded = solver.decode_swmm_datetime(swmm_datetime=45612.564826389)
        expected = datetime(year=2024, month=11, day=16, hour=13, minute=33, second=21)
        self.assertEqual(decoded, expected)


class TestSolverExecution(unittest.TestCase):
    """Tests for running the solver (run_solver, context manager, manual)."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def _clean_outputs(self):
        for p in (self.rpt, self.out):
            if os.path.exists(p):
                os.remove(p)

    def test_run_solver(self):
        self._clean_outputs()
        error = solver.run_solver(
            inp_file=self.inp, rpt_file=self.rpt, out_file=self.out,
        )
        self.assertEqual(error, 0)
        self.assertTrue(os.path.exists(self.rpt))
        self.assertTrue(os.path.exists(self.out))

    def test_run_solver_with_progress_callback(self):
        self._clean_outputs()

        def progress_cb(progress: float):
            assert 0 <= progress <= 1.0

        error = solver.run_solver(
            inp_file=self.inp, rpt_file=self.rpt, out_file=self.out,
            swmm_progress_callback=progress_cb,
        )
        self.assertEqual(error, 0)
        self.assertTrue(os.path.exists(self.rpt))
        self.assertTrue(os.path.exists(self.out))

    def test_run_solver_invalid_inp_file(self):
        with self.assertRaises(Exception) as ctx:
            solver.run_solver(
                inp_file=example_solver_data.NON_EXISTENT_INPUT_FILE,
                rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
                out_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".out"),
            )
        self.assertIn("ERROR 303: cannot open input file.", str(ctx.exception))

    def test_run_without_context_manager(self):
        self._clean_outputs()
        swmm_solver = solver.Solver(
            inp_file=self.inp, rpt_file=self.rpt, out_file=self.out,
        )
        swmm_solver.execute()
        self.assertTrue(os.path.exists(self.rpt))
        self.assertTrue(os.path.exists(self.out))

    def test_run_without_context_manager_step_by_step(self):
        self._clean_outputs()
        swmm_solver = solver.Solver(
            inp_file=self.inp, rpt_file=self.rpt, out_file=self.out,
        )
        swmm_solver.initialize()
        while swmm_solver.solver_state != solver.SolverState.FINISHED:
            swmm_solver.step()
        swmm_solver.finalize()
        self.assertTrue(os.path.exists(self.rpt))
        self.assertTrue(os.path.exists(self.out))

    def test_run_solver_with_context_manager(self):
        with solver.Solver(
            inp_file=self.inp,
            rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
            out_file=self.out,
        ) as swmm_solver:
            swmm_solver.start()
            for _ in swmm_solver:
                pass


class TestSolverTimeAttributes(unittest.TestCase):
    """Tests for solver timing properties."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_get_time_attributes(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self.assertEqual(s.start_datetime, datetime(1998, 1, 1))
            self.assertEqual(s.end_datetime, datetime(1998, 1, 1, 6))
            self.assertEqual(s.report_start_datetime, datetime(1998, 1, 1))

    def test_set_time_attributes(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            start = datetime(2000, 1, 2)
            end = datetime(2000, 1, 2, 2)
            s.start_datetime = start
            s.end_datetime = end
            s.report_start_datetime = start
            s.start()
            for _ in s:
                self.assertTrue(start <= s.current_datetime <= end)


class TestSolverObjectQueries(unittest.TestCase):
    """Tests for object count, name, and index lookups."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def test_get_object_count(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self.assertEqual(s.get_object_count(solver.SWMMObjects.RAIN_GAGE), 1)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.SUBCATCHMENT), 7)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.NODE), 12)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.LINK), 11)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.AQUIFER), 0)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.POLLUTANT), 1)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.LANDUSE), 4)
            self.assertEqual(s.get_object_count(solver.SWMMObjects.TIMESERIES), 3)

            with self.assertRaises(solver.SWMMSolverException):
                s.get_object_count(solver.SWMMObjects.SYSTEM)

    def test_get_object_names(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self.assertListEqual(
                s.get_object_names(solver.SWMMObjects.RAIN_GAGE), ["RainGage"]
            )
            self.assertListEqual(
                s.get_object_names(solver.SWMMObjects.SUBCATCHMENT),
                ["S1", "S2", "S3", "S4", "S5", "S6", "S7"],
            )
            self.assertListEqual(
                s.get_object_names(solver.SWMMObjects.NODE),
                ["J1", "J2", "J3", "J4", "J5", "J6", "J7", "J8", "J9", "J10", "J11", "O1"],
            )
            self.assertListEqual(
                s.get_object_names(solver.SWMMObjects.LINK),
                ["C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10", "C11"],
            )

    def test_get_object_index(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self.assertEqual(s.get_object_index(solver.SWMMObjects.RAIN_GAGE, "RainGage"), 0)
            self.assertEqual(s.get_object_index(solver.SWMMObjects.SUBCATCHMENT, "S2"), 1)
            self.assertEqual(s.get_object_index(solver.SWMMObjects.NODE, "J6"), 5)
            self.assertEqual(s.get_object_index(solver.SWMMObjects.LINK, "C10"), 9)


class TestSolverGetSetValues(unittest.TestCase):
    """Tests for get_value / set_value on gages, subcatchments, nodes, links."""

    def setUp(self):
        self.inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
        self.rpt = self.inp.replace(".inp", ".rpt")
        self.out = self.inp.replace(".inp", ".out")

    def _run_steps(self, s, n=12):
        for _ in range(n):
            s.step()

    def test_get_gage_value(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.RAIN_GAGE,
                property_type=solver.SWMMRainGageProperties.GAGE_TOTAL_PRECIPITATION,
                index=0,
            )
            self.assertAlmostEqual(val / 12.0, 0.3)

    def test_set_gage_value(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            s.set_value(
                object_type=solver.SWMMObjects.RAIN_GAGE,
                property_type=solver.SWMMRainGageProperties.GAGE_RAINFALL,
                index=0, value=3.6,
            )
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.RAIN_GAGE,
                property_type=solver.SWMMRainGageProperties.GAGE_TOTAL_PRECIPITATION,
                index=0,
            )
            self.assertAlmostEqual(val, 3.6)

    def test_get_subcatchment_value(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.RUNOFF,
                index=1,
            )
            self.assertAlmostEqual(val, 17.527141504933294)

    def test_set_subcatchment_value(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.set_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.WIDTH,
                index=1, value=100.0,
            )
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.WIDTH,
                index=1,
            )
            self.assertAlmostEqual(val, 100.0)

    def test_get_subcatchment_initial_buildup(self):
        with solver.Solver(inp_file=self.inp, rpt_file=self.rpt, out_file=self.out) as s:
            s.start()
            val = s.get_value(
                object_type=solver.SWMMObjects.SUBCATCHMENT,
                property_type=solver.SWMMSubcatchmentProperties.POLLUTANT_BUILDUP,
                index=0, sub_index=0,
            )
            self.assertEqual(val, 80.0)

    def test_get_node_value(self):
        with solver.Solver(
            inp_file=self.inp,
            rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
            out_file=self.out,
        ) as s:
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.NODE,
                property_type=solver.SWMMNodeProperties.TOTAL_INFLOW,
                index=5,
            )
            self.assertAlmostEqual(val, 58.58717843671191)

    def test_set_node_value(self):
        with solver.Solver(
            inp_file=self.inp,
            rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
            out_file=self.out,
        ) as s:
            s.set_value(
                object_type=solver.SWMMObjects.NODE,
                property_type=solver.SWMMNodeProperties.INVERT_ELEVATION,
                index=5, value=10.0,
            )
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.NODE,
                property_type=solver.SWMMNodeProperties.INVERT_ELEVATION,
                index=5,
            )
            self.assertAlmostEqual(val, 10.0)

    def test_get_link_value(self):
        with solver.Solver(
            inp_file=self.inp,
            rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
            out_file=self.out,
        ) as s:
            s.start()
            self._run_steps(s)
            by_idx = s.get_value(
                object_type=solver.SWMMObjects.LINK,
                property_type=solver.SWMMLinkProperties.FLOW,
                index=9,
            )
            by_name = s.get_value(
                object_type=solver.SWMMObjects.LINK,
                property_type=solver.SWMMLinkProperties.FLOW,
                index="C10",
            )
            self.assertAlmostEqual(by_idx, 102.01283173880869)
            self.assertAlmostEqual(by_idx, by_name)

    def test_set_link_value(self):
        with solver.Solver(
            inp_file=self.inp,
            rpt_file=example_solver_data.NON_EXISTENT_INPUT_FILE.replace(".inp", ".rpt"),
            out_file=self.out,
        ) as s:
            s.set_value(
                object_type=solver.SWMMObjects.LINK,
                property_type=solver.SWMMLinkProperties.START_NODE_OFFSET,
                index=9, value=1.0,
            )
            s.start()
            self._run_steps(s)
            val = s.get_value(
                object_type=solver.SWMMObjects.LINK,
                property_type=solver.SWMMLinkProperties.START_NODE_OFFSET,
                index=9,
            )
            self.assertAlmostEqual(val, 1.0)
