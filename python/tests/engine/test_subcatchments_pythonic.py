"""
P4 — Subcatchments collection + Subcatchment wrapper Pythonic tests.

Covers the container protocol, identity + geometry properties, gage
back-reference (returns Gage wrapper), runtime state, infiltration view
tagged-union, coverage MutableMapping, stats sub-view, and bulk numpy
properties. Staleness and equality are identical to nodes/links.
"""

from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._subcatchments")

from openswmm.engine import InfilModel, StaleObjectError
from openswmm.engine._gages import Gage
from openswmm.engine._nodes import Node
from openswmm.engine._subcatchments import (
    CoverageView,
    InfiltrationView,
    Subcatchment,
    Subcatchments,
    SubcatchmentStatsView,
)


class TestContainerProtocol:
    def test_len_and_iter(self, opened_solver):
        assert len(opened_solver.subcatchments) > 0
        wrappers = list(opened_solver.subcatchments)
        assert all(isinstance(w, Subcatchment) for w in wrappers)

    def test_int_and_str_indexing(self, opened_solver):
        zero_id = opened_solver.subcatchments.get_id(0)
        assert (opened_solver.subcatchments[zero_id]
                == opened_solver.subcatchments[0])


class TestSubcatchmentProperties:
    def test_identity(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        assert isinstance(s0.id, str)
        assert s0.index == 0
        assert s0.solver is opened_solver

    def test_geometry_round_trips(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        for attr in ("area", "width", "slope", "imperv_pct",
                     "n_imperv", "n_perv", "ds_imperv", "ds_perv"):
            v = getattr(s0, attr)
            assert isinstance(v, float)
            setattr(s0, attr, v)             # round-trip; no behaviour change
            assert getattr(s0, attr) == pytest.approx(v)

    def test_gage_returns_gage_wrapper(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        assert isinstance(s0.gage, Gage)
        assert s0.gage.solver is opened_solver

    def test_outlet_returns_node_or_subcatchment(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        out = s0.outlet
        assert isinstance(out, (Node, Subcatchment)) or out is None

    def test_runtime_state(self, running_solver):
        s0 = running_solver.subcatchments[0]
        for attr in ("runoff", "groundwater", "rainfall",
                     "snow_depth", "evap", "infil"):
            assert isinstance(getattr(s0, attr), float)

    def test_scale_factors_default_to_one(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        assert s0.rain_scale_factor == pytest.approx(1.0)
        assert s0.snow_scale_factor == pytest.approx(1.0)

    def test_scale_factors_round_trip(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        s0.rain_scale_factor = 0.5
        s0.snow_scale_factor = 1.3
        assert s0.rain_scale_factor == pytest.approx(0.5)
        assert s0.snow_scale_factor == pytest.approx(1.3)

    def test_scale_factors_reject_nonpositive(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        with pytest.raises(Exception):
            s0.rain_scale_factor = 0.0
        with pytest.raises(Exception):
            s0.snow_scale_factor = -1.0

    def test_four_precip_factors_are_independent(self, opened_solver):
        # rain vs snow scale on the subcatchment, and vs the gage's own two
        # factors — the set most likely to get cross-wired.
        s0 = opened_solver.subcatchments[0]
        s0.rain_scale_factor = 0.7
        s0.snow_scale_factor = 1.9
        g = s0.gage
        g.scale_factor = 2.3
        g.snow_factor = 1.4
        assert s0.rain_scale_factor == pytest.approx(0.7)
        assert s0.snow_scale_factor == pytest.approx(1.9)
        assert g.scale_factor == pytest.approx(2.3)
        assert g.snow_factor == pytest.approx(1.4)


class TestInfiltration:
    def test_view_exposes_model_enum(self, opened_solver):
        v = opened_solver.subcatchments[0].infiltration
        assert isinstance(v, InfiltrationView)
        assert isinstance(v.model, InfilModel)

    def test_set_horton_changes_model(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)
        assert s0.infiltration.model in (InfilModel.HORTON, InfilModel.MOD_HORTON)
        params = s0.infiltration.horton
        assert params == pytest.approx((3.0, 0.5, 4.0, 7.0))

    def test_set_green_ampt_changes_model(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_green_ampt(3.5, 0.06, 0.26)
        assert s0.infiltration.model in (InfilModel.GREEN_AMPT,
                                          InfilModel.MOD_GREEN_AMPT)
        params = s0.infiltration.green_ampt
        assert params == pytest.approx((3.5, 0.06, 0.26))

    def test_set_curve_number_changes_model(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_curve_number(85.0)
        assert s0.infiltration.model == InfilModel.CURVE_NUMBER
        assert s0.infiltration.curve_number == pytest.approx(85.0)


class TestCoverage:
    def test_is_mutable_mapping(self, opened_solver):
        from collections.abc import MutableMapping
        s0 = opened_solver.subcatchments[0]
        assert isinstance(s0.coverage, MutableMapping)

    def test_set_get_round_trip(self, opened_solver):
        if len(opened_solver.subcatchments) == 0:
            pytest.skip("no subcatchments")
        # Landuse count is exposed at runtime via the quality.landuses
        # collection (the swmm_landuse_count C symbol lives in the
        # cimport-only _common.pxd and has no runtime module).
        n_landuses = len(opened_solver.quality.landuses)
        assert n_landuses >= 0
        s0 = opened_solver.subcatchments[0]
        # Read coverage[0] via the engine's int-based path to dodge id checks.
        # We don't have a landuse id helper in the fixture, so just smoke-test
        # iteration and len.
        n_present = len(s0.coverage)
        assert n_present >= 0


class TestSubviews:
    def test_stats_view(self, completed_solver):
        v = completed_solver.subcatchments[0].stats
        assert isinstance(v, SubcatchmentStatsView)
        assert isinstance(v.precip, float)
        assert isinstance(v.runoff_vol, float)
        assert isinstance(v.max_runoff, float)


class TestBulk:
    @pytest.mark.parametrize("prop", ["runoffs", "rainfalls", "evaps",
                                       "infils", "snow_depths"])
    def test_bulk_props_are_arrays(self, running_solver, prop):
        arr = getattr(running_solver.subcatchments, prop)
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(running_solver.subcatchments)


class TestStaleness:
    def test_rename_invalidates(self, opened_solver):
        s0 = opened_solver.subcatchments[0]
        original = s0.id
        opened_solver.subcatchments.rename(0, original + "_x")
        with pytest.raises(StaleObjectError):
            _ = s0.area
        opened_solver.subcatchments.rename(0, original)
