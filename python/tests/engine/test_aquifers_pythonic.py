"""Aquifers collection coverage (gap-review 2026-07-06).

Exercises ``solver.aquifers`` — previously untouched by the test suite
(class-level coverage gap identified in
``plans/api_gap_review_2026-07-06/``). Uses the ``b8_aq_probe`` model,
which carries an ``[AQUIFERS]`` section.
"""
from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._subcatchments")

from openswmm.engine import AquiferParam, Aquifers, Solver

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "legacy", "output", "b8_aq_probe.inp")


@pytest.fixture
def aq_solver(tmp_path):
    """An opened Solver on a model with an [AQUIFERS] section."""
    s = Solver(_MODEL, str(tmp_path / "aq.rpt"), str(tmp_path / "aq.out"))
    s.open()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


class TestAquifersContainer:
    def test_property_type(self, aq_solver):
        assert isinstance(aq_solver.aquifers, Aquifers)

    def test_len_positive(self, aq_solver):
        assert len(aq_solver.aquifers) > 0

    def test_iter_yields_ids(self, aq_solver):
        ids = list(aq_solver.aquifers)
        assert ids and all(isinstance(i, str) for i in ids)
        assert len(ids) == len(aq_solver.aquifers)

    def test_id_index_roundtrip(self, aq_solver):
        aq = aq_solver.aquifers
        name = aq.get_id(0)
        assert aq.get_index(name) == 0

    def test_contains(self, aq_solver):
        first = next(iter(aq_solver.aquifers))
        assert first in aq_solver.aquifers
        assert "NO_SUCH_AQUIFER_xyz" not in aq_solver.aquifers


class TestAquiferParams:
    def test_get_param_returns_float(self, aq_solver):
        first = next(iter(aq_solver.aquifers))
        val = aq_solver.aquifers.get_param(first, AquiferParam.POROSITY)
        assert isinstance(val, float)

    def test_set_param_roundtrip(self, aq_solver):
        aq = aq_solver.aquifers
        first = next(iter(aq))
        # Pick a porosity strictly between field capacity and 1 so the value
        # is valid regardless of the model's exact soil parameters.
        fc = aq.get_param(first, AquiferParam.FIELD_CAPACITY)
        target = (fc + 1.0) / 2.0
        aq.set_param(first, AquiferParam.POROSITY, target)
        assert aq.get_param(first, AquiferParam.POROSITY) == pytest.approx(target)

    def test_param_by_index_matches_by_name(self, aq_solver):
        aq = aq_solver.aquifers
        name = aq.get_id(0)
        assert aq.get_param(0, AquiferParam.POROSITY) == pytest.approx(
            aq.get_param(name, AquiferParam.POROSITY))

    def test_evap_pattern_is_str(self, aq_solver):
        first = next(iter(aq_solver.aquifers))
        assert isinstance(aq_solver.aquifers.get_evap_pattern(first), str)
