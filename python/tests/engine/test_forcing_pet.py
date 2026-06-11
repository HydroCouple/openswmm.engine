"""Subcatchment PET prescription tests (refactored engine).

Covers docs/SUBCATCHMENT_PET_PRESCRIPTION_PLAN.md section 6 for the
refactored API: override takes effect, capping, mass balance, ADD mode,
persist vs one-shot reset, clearing, the climate evap rate getter, and
adjustment composition.

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). Report/output files are written to ``tests/engine/output``
so they remain reviewable after the run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

from openswmm.engine import Solver, ForcingMode

# The bundled site-drainage model uses CONSTANT evaporation of 0.0 in/day
# (DRY_ONLY NO, US units) and a 2-yr design storm beginning at sim start,
# so any non-zero evaporation observed below is attributable to the
# prescribed PET alone.
_DATA_DIR = os.path.join(
    os.path.dirname(os.path.dirname(__file__)), "data", "solver")
_INP = os.path.join(_DATA_DIR, "site_drainage_example.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

# Steps to run before asserting, so the storm has wetted all subareas.
_SPINUP_STEPS = 30
_PET = 2.4  # in/day


# The site model uses WET_STEP 1 min over a 15 s ROUTING_STEP, so the runoff
# clock (which is where subcatchment evap is evaluated) fires only on every
# 4th routing step. The single-step assertions below need a prescription to
# be observable on the very next step, so the test model aligns WET_STEP to
# the routing step.
_WET_STEP_ALIGN = ("WET_STEP             00:01:00",
                   "WET_STEP             00:00:15")


@pytest.fixture
def pet_solver(request):
    """A running Solver whose rpt/out files land in a reviewable folder."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, f"pet_{request.node.name}")
    inp = _derived_model("pet_base.inp", [_WET_STEP_ALIGN])
    s = Solver(inp, base + ".rpt", base + ".out")
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


def _spinup(s, n=_SPINUP_STEPS):
    for _ in range(n):
        s.step()


class TestOverride:
    def test_override_takes_effect(self, pet_solver):
        """Case 1: prescribed PET drives evap loss; untouched subcatch stays dry."""
        s = pet_solver
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.step()
        s1 = s.subcatchments["S1"]
        s2 = s.subcatchments["S2"]
        # Storm is active, so ponded depth >> PET rate: loss equals the rate.
        assert s1.evap == pytest.approx(_PET, rel=1e-3)
        # Climate evap is 0.0, so the unforced subcatchment shows none.
        assert s2.evap == pytest.approx(0.0, abs=1e-12)

    def test_add_mode(self, pet_solver):
        """Case 5: ADD augments the climate rate (0.0 here)."""
        s = pet_solver
        _spinup(s)
        s.forcing.subcatchment_evap(
            "S1", 1.5, mode=ForcingMode.ADD, persist=True)
        s.step()
        assert s.subcatchments["S1"].evap == pytest.approx(1.5, rel=1e-3)

    def test_one_shot_resets_after_one_step(self, pet_solver):
        """Case 6: persist=False affects exactly one step."""
        s = pet_solver
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=False)
        s.step()
        assert s.subcatchments["S1"].evap == pytest.approx(_PET, rel=1e-3)
        s.step()
        assert s.subcatchments["S1"].evap == pytest.approx(0.0, abs=1e-12)

    def test_clear_reverts_to_climate(self, pet_solver):
        """Case 4: clearing reverts to the climate-derived rate (0.0)."""
        s = pet_solver
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.step()
        assert s.subcatchments["S1"].evap > 0.0
        s.forcing.clear_all()
        s.step()
        assert s.subcatchments["S1"].evap == pytest.approx(0.0, abs=1e-12)


class TestCappingAndMassBalance:
    def test_capping_to_available_water(self, pet_solver):
        """Case 2: an extreme PET cannot evaporate more than is available."""
        s = pet_solver
        extreme = 1.0e6  # in/day — far beyond any plausible ponded volume
        s.forcing.subcatchment_evap("S1", extreme, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        s1 = s.subcatchments["S1"]
        # Loss is capped by available water: strictly below the demand,
        # and never negative.
        assert 0.0 <= s1.evap < extreme

    def test_mass_balance_continuity(self, pet_solver):
        """Case 3: persistent prescription keeps runoff continuity sound."""
        s = pet_solver
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.forcing.subcatchment_evap("S2", _PET, persist=True)
        while s.step():
            pass
        s.end()
        err = s.mass_balance.runoff_continuity_error
        assert abs(err) < 0.5

    def test_evap_appears_in_runoff_totals(self, pet_solver):
        """Case 3 (totals): forced evaporation shows up in the system totals."""
        from openswmm.engine import RunoffTotal
        s = pet_solver
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        for _ in range(2 * _SPINUP_STEPS):
            s.step()
        total_evap = s.mass_balance.runoff_total(RunoffTotal.EVAP)
        assert total_evap > 0.0


class TestClimateRateGetter:
    def test_getter_matches_model_climate(self, pet_solver):
        """L9/R8: the model's constant evap is 0.0 in/day."""
        s = pet_solver
        _spinup(s)
        assert s.forcing.climate_evap_rate() == pytest.approx(0.0, abs=1e-12)

    def test_adjustment_composition_round_trip(self, pet_solver):
        """Case 13: read rate, apply caller-side adjustment, prescribe result.

        The engine applies the composed rate verbatim — no further
        engine-side adjustment.
        """
        s = pet_solver
        _spinup(s)
        base = s.forcing.climate_evap_rate()  # 0.0 for this model
        composed = base * 0.8 + 1.2           # caller-side logic
        s.forcing.subcatchment_evap("S1", composed, persist=True)
        s.step()
        assert s.subcatchments["S1"].evap == pytest.approx(composed, rel=1e-3)


def _derived_model(name, replacements):
    """Write a modified copy of the site model to the reviewable output dir."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_INP) as f:
        text = f.read()
    for old, new in replacements:
        assert old in text, f"expected {old!r} in template model"
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


class TestDryOnlyBypass:
    def test_prescription_bypasses_dry_only(self):
        """Case 7: prescribed PET evaporates even when DRY_ONLY suppresses
        the climate rate during rainfall."""
        inp = _derived_model("pet_dry_only.inp", [
            ("CONSTANT         0.0", "CONSTANT         5.0"),
            ("DRY_ONLY         NO", "DRY_ONLY         YES"),
            _WET_STEP_ALIGN,
        ])
        base = os.path.join(_OUT_DIR, "pet_dry_only")
        s = Solver(inp, base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        try:
            _spinup(s)  # storm is active → DRY_ONLY zeroes climate evap
            s.step()
            assert s.subcatchments["S2"].evap == pytest.approx(0.0, abs=1e-12)
            s.forcing.subcatchment_evap("S1", _PET, persist=True)
            s.step()
            assert s.subcatchments["S1"].evap == pytest.approx(_PET, rel=1e-3)
        finally:
            s.close()
            s.destroy()


class TestSIUnits:
    def test_si_prescription_round_trips_in_mm_day(self):
        """Case 9: on an SI model the API accepts and reports mm/day."""
        inp = _derived_model("pet_si_units.inp", [
            ("FLOW_UNITS           CFS", "FLOW_UNITS           CMS"),
            _WET_STEP_ALIGN,
        ])
        base = os.path.join(_OUT_DIR, "pet_si_units")
        pet_mm_day = 5.0
        s = Solver(inp, base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        try:
            _spinup(s)
            s.forcing.subcatchment_evap("S1", pet_mm_day, persist=True)
            s.step()
            assert s.subcatchments["S1"].evap == pytest.approx(
                pet_mm_day, rel=1e-3)
        finally:
            s.close()
            s.destroy()


class TestErrorPaths:
    def test_bad_subcatchment_raises(self, pet_solver):
        """Case 12: unknown ids and bad indices are rejected."""
        s = pet_solver
        with pytest.raises(Exception):
            s.forcing.subcatchment_evap("NO_SUCH_SUBCATCH", _PET)

    def test_bad_mode_raises(self, pet_solver):
        s = pet_solver
        with pytest.raises(Exception):
            s.forcing.subcatchment_evap("S1", _PET, mode=99)
