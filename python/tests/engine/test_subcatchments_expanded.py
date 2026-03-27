"""Tests for expanded :class:`openswmm.engine.Subcatchments` bindings."""

import numpy as np
import pytest

from openswmm.engine import Subcatchments


class TestSubcatchmentsIdentity:
    """Identity methods."""

    def test_get_id(self, subcatchments):
        name = subcatchments.get_id(0)
        assert isinstance(name, str)
        assert len(name) > 0

    def test_roundtrip_id_index(self, subcatchments):
        name = subcatchments.get_id(0)
        idx = subcatchments.get_index(name)
        assert idx == 0


class TestSubcatchmentsPropertyGetters:
    """Subcatchment property reads."""

    def test_get_area(self, subcatchments):
        v = subcatchments.get_area(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_width(self, subcatchments):
        v = subcatchments.get_width(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_slope(self, subcatchments):
        v = subcatchments.get_slope(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_imperv_pct(self, subcatchments):
        v = subcatchments.get_imperv_pct(0)
        assert isinstance(v, float)
        assert 0.0 <= v <= 100.0

    def test_get_outlet(self, subcatchments):
        v = subcatchments.get_outlet(0)
        assert isinstance(v, int)

    def test_get_n_imperv(self, subcatchments):
        v = subcatchments.get_n_imperv(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_n_perv(self, subcatchments):
        v = subcatchments.get_n_perv(0)
        assert isinstance(v, float)
        assert v > 0.0

    def test_get_ds_imperv(self, subcatchments):
        v = subcatchments.get_ds_imperv(0)
        assert isinstance(v, float)

    def test_get_ds_perv(self, subcatchments):
        v = subcatchments.get_ds_perv(0)
        assert isinstance(v, float)

    def test_get_gage(self, subcatchments):
        v = subcatchments.get_gage(0)
        assert isinstance(v, int)


class TestSubcatchmentsInfiltration:
    """Infiltration model queries."""

    def test_get_infil_model(self, subcatchments):
        v = subcatchments.get_infil_model(0)
        assert isinstance(v, int)
        assert v >= 0


class TestSubcatchmentsState:
    """Runtime state getters."""

    def test_get_rainfall(self, stepped_subcatchments):
        v = stepped_subcatchments.get_rainfall(0)
        assert isinstance(v, float)

    def test_get_evap(self, stepped_subcatchments):
        v = stepped_subcatchments.get_evap(0)
        assert isinstance(v, float)

    def test_get_infil(self, stepped_subcatchments):
        v = stepped_subcatchments.get_infil(0)
        assert isinstance(v, float)

    def test_get_snow_depth(self, stepped_subcatchments):
        v = stepped_subcatchments.get_snow_depth(0)
        assert isinstance(v, float)
        assert v >= 0.0


class TestSubcatchmentsStatistics:
    """Subcatchment statistics after simulation."""

    def test_stat_precip(self, completed_solver):
        sc = Subcatchments(completed_solver)
        v = sc.get_stat_precip(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_stat_runoff_vol(self, completed_solver):
        sc = Subcatchments(completed_solver)
        v = sc.get_stat_runoff_vol(0)
        assert isinstance(v, float)
        assert v >= 0.0

    def test_stat_max_runoff(self, completed_solver):
        sc = Subcatchments(completed_solver)
        v = sc.get_stat_max_runoff(0)
        assert isinstance(v, float)
        assert v >= 0.0


class TestSubcatchmentsBulk:
    """Bulk access methods."""

    def test_get_runoff_bulk(self, stepped_subcatchments):
        arr = stepped_subcatchments.get_runoff_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_subcatchments.count(),)
        assert arr.dtype == np.float64


class TestSubcatchmentsStringAccess:
    """Access by string ID."""

    def test_get_area_by_name(self, subcatchments):
        name = subcatchments.get_id(0)
        v = subcatchments.get_area(name)
        assert isinstance(v, float)
        assert v > 0.0

    def test_nonexistent_name_raises(self, subcatchments):
        with pytest.raises(KeyError, match="not found"):
            subcatchments.get_area("NONEXISTENT_SC")
