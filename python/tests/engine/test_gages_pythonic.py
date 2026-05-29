"""
P4 — Gages collection + Gage wrapper Pythonic surface tests.

Smaller scope than nodes/links: no sub-views, just identity + a few
typed properties + bulk rainfall + the same int|str / staleness /
equality contracts.
"""

from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._gages")

from openswmm.engine import GageDataSource, GageRainType, StaleObjectError
from openswmm.engine._gages import Gage, Gages


class TestContainerProtocol:
    def test_len_and_iter(self, opened_solver):
        n = len(opened_solver.gages)
        assert n > 0
        wrappers = list(opened_solver.gages)
        assert all(isinstance(w, Gage) for w in wrappers)
        assert len(wrappers) == n

    def test_int_and_str_indexing(self, opened_solver):
        zero_id = opened_solver.gages.get_id(0)
        assert opened_solver.gages[zero_id] == opened_solver.gages[0]

    def test_unknown_id_raises_keyerror(self, opened_solver):
        with pytest.raises(KeyError):
            _ = opened_solver.gages["NO_SUCH_GAGE_xyz"]


class TestGageProperties:
    def test_identity(self, opened_solver):
        g0 = opened_solver.gages[0]
        assert isinstance(g0.id, str)
        assert g0.index == 0
        assert g0.solver is opened_solver

    def test_typed_enums(self, opened_solver):
        g0 = opened_solver.gages[0]
        assert isinstance(g0.rain_type, GageRainType)
        assert isinstance(g0.data_source, GageDataSource)

    def test_rainfall_setter(self, running_solver):
        g0 = running_solver.gages[0]
        g0.rainfall = 12.5
        assert g0.rainfall == pytest.approx(12.5)

    def test_scale_factor_default_is_one(self, opened_solver):
        g0 = opened_solver.gages[0]
        assert g0.scale_factor == pytest.approx(1.0)

    def test_scale_factor_round_trip(self, opened_solver):
        g0 = opened_solver.gages[0]
        g0.scale_factor = 2.5
        assert g0.scale_factor == pytest.approx(2.5)

    def test_scale_factor_rejects_nonpositive(self, opened_solver):
        g0 = opened_solver.gages[0]
        with pytest.raises(Exception):
            g0.scale_factor = 0.0
        with pytest.raises(Exception):
            g0.scale_factor = -1.0
        # Value must remain at its prior default.
        assert g0.scale_factor == pytest.approx(1.0)

    def test_scale_factor_settable_while_running(self, running_solver):
        g0 = running_solver.gages[0]
        g0.scale_factor = 3.0
        assert g0.scale_factor == pytest.approx(3.0)


class TestBulk:
    def test_rainfalls_array(self, running_solver):
        arr = running_solver.gages.rainfalls
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.float64
        assert arr.shape[0] == len(running_solver.gages)

    def test_ids_array(self, opened_solver):
        ids = opened_solver.gages.ids
        assert ids.dtype == object
        assert list(ids) == [g.id for g in opened_solver.gages]


class TestStaleness:
    def test_rename_invalidates(self, opened_solver):
        g0 = opened_solver.gages[0]
        original = g0.id
        opened_solver.gages.rename(0, original + "_x")
        with pytest.raises(StaleObjectError):
            _ = g0.rainfall
        opened_solver.gages.rename(0, original)
