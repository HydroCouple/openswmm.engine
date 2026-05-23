"""Tests for expanded :class:`openswmm.engine.Gages` bindings."""

import numpy as np
import pytest

from openswmm.engine import Gages


class TestGagesIdentity:
    """Gage identity methods."""

    def test_get_id(self, gages):
        name = gages.get_id(0)
        assert isinstance(name, str)
        assert len(name) > 0

    def test_roundtrip_id_index(self, gages):
        name = gages.get_id(0)
        idx = gages.get_index(name)
        assert idx == 0


class TestGagesProperties:
    """Gage property get/set."""

    def test_get_rain_type(self, gages):
        v = gages.get_rain_type(0)
        assert isinstance(v, int)
        assert v >= 0

    def test_get_data_source(self, gages):
        v = gages.get_data_source(0)
        assert isinstance(v, int)
        assert v >= 0


class TestGagesBulk:
    """Bulk rainfall access."""

    def test_get_rainfall_bulk(self, stepped_gages):
        arr = stepped_gages.get_rainfall_bulk()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (stepped_gages.count(),)
        assert arr.dtype == np.float64


# ---------------------------------------------------------------------------
# Rename (new addition)
# ---------------------------------------------------------------------------
class TestGagesRename:
    """rename() changes the gage's string identifier."""

    def test_rename_changes_id(self, opened_solver):
        g = Gages(opened_solver)
        if g.count() == 0:
            pytest.skip("no gages in test model")
        original_name = g.get_id(0)
        new_name = original_name + "_renamed"
        g.rename(0, new_name)
        assert g.get_id(0) == new_name
        # restore
        g.rename(0, original_name)

    def test_rename_by_string_index(self, opened_solver):
        g = Gages(opened_solver)
        if g.count() == 0:
            pytest.skip("no gages in test model")
        original_name = g.get_id(0)
        new_name = original_name + "_v2"
        g.rename(original_name, new_name)
        assert g.get_index(new_name) == 0
        # restore
        g.rename(new_name, original_name)

