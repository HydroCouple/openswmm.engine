"""Climate wrapper coverage (gap-review 2026-07-06).

The ``Climate`` OOP wrapper (``solver.climate``) was never exercised via its
own property surface — climate had only been reached through the forcing /
legacy paths. Round-trips the numeric scalar and monthly / ADC array
properties on the standard site-drainage model (defaults are readable /
writable even without an explicit ``[TEMPERATURE]`` section).
"""
from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._climate")

from openswmm.engine import Climate, Solver

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "data", "solver", "site_drainage_example.inp")


@pytest.fixture
def clim_solver(tmp_path):
    s = Solver(_MODEL, str(tmp_path / "cl.rpt"), str(tmp_path / "cl.out"))
    s.open()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


class TestClimateProperty:
    def test_type(self, clim_solver):
        assert isinstance(clim_solver.climate, Climate)

    @pytest.mark.parametrize("attr",
                             ["temp_source", "evap_type", "wind_type", "temp_units"])
    def test_enum_ints_readable(self, clim_solver, attr):
        assert isinstance(getattr(clim_solver.climate, attr), int)


class TestClimateScalarRoundTrip:
    @pytest.mark.parametrize("attr,value", [
        ("elevation", 123.4),
        ("latitude", 41.5),
        ("longitude_correction", -5.0),
        ("snow_temp", 33.0),
        ("ati_weight", 0.5),
        ("neg_melt_ratio", 0.6),
    ])
    def test_scalar_roundtrip(self, clim_solver, attr, value):
        setattr(clim_solver.climate, attr, value)
        assert getattr(clim_solver.climate, attr) == pytest.approx(value)


class TestClimateArrayRoundTrip:
    @pytest.mark.parametrize("attr,n", [
        ("evap_monthly", 12),
        ("pan_coeff", 12),
        ("wind_monthly", 12),
        ("adjust_temperature", 12),
        ("adjust_evaporation", 12),
        ("adjust_rainfall", 12),
        ("adjust_conductivity", 12),
        ("adc_impervious", 10),
        ("adc_pervious", 10),
    ])
    def test_array_roundtrip(self, clim_solver, attr, n):
        # small, strictly-increasing fractional values — valid for ADC curves
        # and harmless for the monthly/adjust arrays.
        vals = [round(0.01 * i, 3) for i in range(n)]
        setattr(clim_solver.climate, attr, vals)
        got = list(getattr(clim_solver.climate, attr))
        assert got == pytest.approx(vals)
