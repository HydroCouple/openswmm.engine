"""Snowpacks collection coverage (gap-review 2026-07-06).

Exercises ``solver.snowpacks`` — previously untouched by the test suite.
Uses ``site_drainage_snow.inp`` which carries a ``[SNOWPACKS]`` section.
"""
from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._subcatchments")

from openswmm.engine import Snowpacks, Solver

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "data", "solver", "site_drainage_snow.inp")

_SURFACE_KEYS = {"cmin", "cmax", "tbase", "fwfrac", "sd0", "fw0", "last"}


@pytest.fixture
def snow_solver(tmp_path):
    """An opened Solver on a model with a [SNOWPACKS] section."""
    s = Solver(_MODEL, str(tmp_path / "sn.rpt"), str(tmp_path / "sn.out"))
    s.open()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


class TestSnowpacksContainer:
    def test_property_type(self, snow_solver):
        assert isinstance(snow_solver.snowpacks, Snowpacks)

    def test_len_and_iter(self, snow_solver):
        ids = list(snow_solver.snowpacks)
        assert len(ids) == len(snow_solver.snowpacks)
        assert len(ids) > 0
        assert all(isinstance(i, str) for i in ids)


class TestSnowpackSurfaces:
    @pytest.mark.parametrize("getter",
                             ["get_plowable", "get_impervious", "get_pervious"])
    def test_surface_dicts(self, snow_solver, getter):
        sp = next(iter(snow_solver.snowpacks))
        d = getattr(snow_solver.snowpacks, getter)(sp)
        assert _SURFACE_KEYS.issubset(d)

    def test_set_plowable_roundtrip(self, snow_solver):
        packs = snow_solver.snowpacks
        sp = next(iter(packs))
        packs.set_plowable(sp, cmin=0.1, cmax=0.5, tbase=0.0,
                           fwfrac=0.1, sd0=0.0, fw0=0.0, last=0.0)
        d = packs.get_plowable(sp)
        assert d["cmin"] == pytest.approx(0.1)
        assert d["cmax"] == pytest.approx(0.5)
        assert d["fwfrac"] == pytest.approx(0.1)

    def test_removal_dict_non_empty(self, snow_solver):
        sp = next(iter(snow_solver.snowpacks))
        d = snow_solver.snowpacks.get_removal(sp)
        assert isinstance(d, dict) and d

    def test_removal_subcatch_is_str(self, snow_solver):
        sp = next(iter(snow_solver.snowpacks))
        assert isinstance(snow_solver.snowpacks.get_removal_subcatch(sp), str)
