"""Tests for new engine API modules: Statistics, Solver options, HotStart expansion."""

import os
import pytest
import numpy as np

from openswmm.engine import Solver, Nodes, Links, Subcatchments, MassBalance


# ---------------------------------------------------------------------------
# Statistics module (via Nodes/Links stat methods)
# ---------------------------------------------------------------------------
class TestStatisticsViaNodes:
    """Statistics accessed through expanded Nodes methods."""

    def test_all_node_stats_after_simulation(self, completed_solver):
        nodes = Nodes(completed_solver)
        n = nodes.count()
        for i in range(n):
            md = nodes.get_stat_max_depth(i)
            assert isinstance(md, float)
            assert md >= 0.0


class TestStatisticsViaLinks:
    """Statistics accessed through expanded Links methods."""

    def test_all_link_stats_after_simulation(self, completed_solver):
        links = Links(completed_solver)
        n = links.count()
        for i in range(n):
            mf = links.get_stat_max_flow(i)
            assert isinstance(mf, float)


class TestStatisticsViaSubcatchments:
    """Statistics accessed through expanded Subcatchments methods."""

    def test_all_subcatch_stats_after_simulation(self, completed_solver):
        sc = Subcatchments(completed_solver)
        n = sc.count()
        for i in range(n):
            p = sc.get_stat_precip(i)
            assert isinstance(p, float)
            assert p >= 0.0


# ---------------------------------------------------------------------------
# Solver options
# ---------------------------------------------------------------------------
class TestSolverOptions:
    """Solver get_option / set_option."""

    def test_get_option_flow_units(self, opened_solver):
        v = opened_solver.get_option("FLOW_UNITS")
        assert isinstance(v, str)
        assert len(v) > 0

    def test_get_crs(self, opened_solver):
        # May or may not have CRS set
        try:
            v = opened_solver.get_crs()
            assert isinstance(v, str)
        except RuntimeError:
            pass  # No CRS defined is OK


# ---------------------------------------------------------------------------
# HotStart expansion
# ---------------------------------------------------------------------------
class TestHotStartExpanded:
    """Hot start file expanded methods."""

    def test_save_and_open_roundtrip(self, solver_files, tmp_path):
        from openswmm.engine import HotStart

        inp, rpt, out = solver_files
        hs_path = str(tmp_path / "test.hs")

        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        for _ in range(10):
            s.step()
        HotStart.save(s, hs_path)
        s.end()

        assert os.path.exists(hs_path)

        with HotStart.open(hs_path) as hs:
            assert hs.node_count() > 0
            assert hs.link_count() > 0
            t = hs.get_sim_time()
            assert isinstance(t, float)
            assert t > 0.0
            assert hs.warning_count() == 0

        s.close()
        s.destroy()


# ---------------------------------------------------------------------------
# Enums coverage
# ---------------------------------------------------------------------------
class TestEnums:
    """Verify all new enums are importable and have expected members."""

    def test_gage_enums(self):
        from openswmm.engine import GageDataSource, GageRainType
        assert GageDataSource.TIMESERIES == 0
        assert GageDataSource.FILE == 1
        assert GageRainType.INTENSITY == 0
        assert GageRainType.CUMULATIVE == 2

    def test_infil_model(self):
        from openswmm.engine import InfilModel
        assert InfilModel.HORTON == 0
        assert InfilModel.CURVE_NUMBER == 4

    def test_outfall_type(self):
        from openswmm.engine import OutfallType
        assert OutfallType.FREE == 0
        assert OutfallType.TIMESERIES == 4

    def test_runoff_routing_totals(self):
        from openswmm.engine import RunoffTotal, RoutingTotal
        assert RunoffTotal.RAINFALL == 0
        assert RunoffTotal.FINALSTORE == 6
        assert RoutingTotal.DRY_WEATHER == 0
        assert RoutingTotal.FINAL_STORAGE == 10

    def test_lid_type(self):
        from openswmm.engine import LidType
        assert LidType.BIO_CELL == 0
        assert LidType.VEGETATIVE_SWALE == 7

    def test_output_var_enums(self):
        from openswmm.engine import OutSubcatchVar, OutNodeVar, OutLinkVar, OutSystemVar
        assert OutSubcatchVar.RAINFALL == 0
        assert OutNodeVar.DEPTH == 0
        assert OutLinkVar.FLOW == 0
        assert OutSystemVar.TEMPERATURE == 0

    def test_buildup_washoff_funcs(self):
        from openswmm.engine import BuildupFunc, WashoffFunc
        assert BuildupFunc.NONE == 0
        assert BuildupFunc.POW == 1
        assert WashoffFunc.EMC == 3

    def test_pattern_type(self):
        from openswmm.engine import PatternType
        assert PatternType.MONTHLY == 0
        assert PatternType.WEEKEND == 3

    def test_concentration_units(self):
        from openswmm.engine import ConcentrationUnits
        assert ConcentrationUnits.MG_PER_L == 0
        assert ConcentrationUnits.COUNT_PER_L == 2
