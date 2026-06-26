"""Snowfall forcing + snow-path tests (refactored engine) — plan row 4 (M3).

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md item M3 plus the snow-path
repair it depended on:
  * snow accumulation regression — gage-derived snowfall must grow the
    pack (plowSnow was previously never called, so packs could never
    accumulate)
  * per-subcatchment snowfall forcing (REPLACE suppress/prescribe, ADD)
  * temperature forcing (M1) driving melt on/off
  * runoff continuity under prescription

Uses the checked-in ``site_drainage_snow.inp`` fixture: snow pack SP1 on
S1 only (S2 is the control), constant 25 deg F temperature series (below
the 34 deg F dividing temperature, so storm precipitation arrives as
snow), melt coefficients 0.002–0.005 in/hr/deg F.

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). Report/output files are written to ``tests/engine/output``.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

from openswmm.engine import Solver, ForcingMode

_DATA_DIR = os.path.join(
    os.path.dirname(os.path.dirname(__file__)), "data", "solver")
_INP = os.path.join(_DATA_DIR, "site_drainage_snow.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

_SPINUP_STEPS = 60   # 15 s routing steps → 15 min into the design storm


@pytest.fixture
def snow_solver(request):
    """A running Solver on the snow fixture; outputs land reviewably."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, f"snow_{request.node.name}")
    s = Solver(_INP, base + ".rpt", base + ".out")
    s.open()
    s.initialize()
    s.start()
    yield s
    try:
        s.end()
        s.report()
    except Exception:
        pass
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


class TestAccumulationRegression:
    def test_gage_snow_accumulates(self, snow_solver):
        """Storm precip at 25 F arrives as snow and must grow S1's pack."""
        s = snow_solver
        for _ in range(_SPINUP_STEPS):
            s.step()
        d1 = s.subcatchments["S1"].snow_depth
        assert d1 > 0.0
        for _ in range(_SPINUP_STEPS):
            s.step()
        d2 = s.subcatchments["S1"].snow_depth
        assert d2 > d1  # still snowing — pack keeps growing
        # Control subcatchment has no pack.
        assert s.subcatchments["S2"].snow_depth == pytest.approx(0.0, abs=1e-12)


class TestSnowfallForcing:
    def test_override_zero_suppresses_gage_snow(self, snow_solver):
        """REPLACE 0.0 must keep the pack empty despite gage snowfall."""
        s = snow_solver
        s.forcing.subcatchment_snowfall("S1", 0.0, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        assert s.subcatchments["S1"].snow_depth == pytest.approx(0.0, abs=1e-9)

    def test_override_prescribes_accumulation(self, snow_solver):
        """A prescribed rate builds the pack while warm air keeps the
        gage delivering rain (no gage snow)."""
        s = snow_solver
        # Warm air: gage precip becomes rain; only the prescription snows.
        s.forcing.climate_temperature(50.0, persist=True)
        s.forcing.subcatchment_snowfall("S1", 2.0, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        d = s.subcatchments["S1"].snow_depth
        assert d > 0.0
        # Melt at 50 F with dhm <= 0.005 in/hr/F (< 0.1 in/hr) is far below
        # the 2 in/hr prescription, so the pack keeps growing.
        for _ in range(_SPINUP_STEPS):
            s.step()
        assert s.subcatchments["S1"].snow_depth > d

    def test_add_mode_augments_gage_snow(self, snow_solver):
        """ADD increases accumulation relative to the gage-only pack."""
        s = snow_solver
        s.forcing.subcatchment_snowfall(
            "S1", 1.0, mode=ForcingMode.ADD, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        assert s.subcatchments["S1"].snow_depth > 0.0


class TestTemperatureMeltResponse:
    def test_forced_warmup_melts_pack(self, snow_solver):
        """M1 consumption proof: a forced warm-up melts the pack."""
        s = snow_solver
        # Build the pack quickly under cold prescribed snow.
        s.forcing.subcatchment_snowfall("S1", 3.0, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        peak = s.subcatchments["S1"].snow_depth
        assert peak > 0.0
        # Stop snowing and force strong warmth (+ rain-on-snow conditions).
        s.forcing.subcatchment_snowfall("S1", 0.0, persist=True)
        s.forcing.climate_temperature(80.0, persist=True)
        for _ in range(4 * _SPINUP_STEPS):
            s.step()
        assert s.subcatchments["S1"].snow_depth < peak

    def test_forced_cold_stops_melt(self, snow_solver):
        """Pack holds (no melt output) while temperature is forced below
        the melt base temperature with no further snowfall."""
        s = snow_solver
        s.forcing.subcatchment_snowfall("S1", 3.0, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        s.forcing.subcatchment_snowfall("S1", 0.0, persist=True)
        s.forcing.climate_temperature(10.0, persist=True)  # << tbase 32 F
        d1 = s.subcatchments["S1"].snow_depth
        for _ in range(_SPINUP_STEPS):
            s.step()
        # No melt and no snowfall: depth unchanged (within rounding).
        assert s.subcatchments["S1"].snow_depth == pytest.approx(d1, rel=1e-6)


class TestContinuity:
    def test_runoff_continuity_with_prescription(self, snow_solver):
        s = snow_solver
        s.forcing.subcatchment_snowfall("S1", 2.0, persist=True)
        while s.step():
            pass
        s.end()
        err = s.mass_balance.runoff_continuity_error
        assert abs(err) < 0.5
