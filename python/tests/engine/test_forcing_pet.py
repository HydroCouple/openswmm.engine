"""Subcatchment PET prescription tests (refactored engine).

Covers docs/SUBCATCHMENT_PET_PRESCRIPTION_PLAN.md section 6 for the
refactored API: override takes effect, capping, mass balance, ADD mode,
persist vs one-shot reset, clearing, the climate evap rate getter, and
adjustment composition.

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). Report/output files are written to reviewable folders
(``tests/engine/output`` and ``tests/_artifacts``) so they remain
inspectable after the run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

from openswmm.engine import Solver, ForcingMode

from tests._paths import artifact_dir
from tests.engine._solver_cases import EngineSolverCase

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


# The site model evaluates subcatchment evaporation on the RUNOFF clock, which
# is independent of the routing clock. Two things must hold for a prescription
# to be observable on the very next step():
#   1. WET_STEP must equal ROUTING_STEP so a runoff step fits inside each
#      routing step (otherwise runoff fires only every 4th step).
#   2. VARIABLE_STEP must be disabled so the routing step is the fixed
#      ROUTING_STEP and stays in 1:1 lockstep with the runoff clock. With the
#      default CFL-limited variable step, routing steps are shorter and
#      irregular, so runoff fires only on some steps and a forcing change is
#      observed one or more steps late.
# These are baked into every derived model below via _derived_model().
_DETERMINISTIC_RUNOFF = (
    ("WET_STEP             00:01:00", "WET_STEP             00:00:15"),
    ("VARIABLE_STEP        0.75", "VARIABLE_STEP        0.0"),
)


class PetSolverCase(EngineSolverCase):
    """Base case providing a running Solver for PET prescription tests."""

    def pet_solver(self):
        """A running Solver whose rpt/out files land in a reviewable folder."""
        base = os.path.join(artifact_dir(self), "pet")
        inp = _derived_model("pet_base.inp")
        s = Solver(inp, base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        self.addCleanup(self._end_close_destroy, s)
        return s


def _spinup(s, n=_SPINUP_STEPS):
    for _ in range(n):
        s.step()


class TestOverride(PetSolverCase):
    def test_override_takes_effect(self):
        """Case 1: prescribed PET drives evap loss; untouched subcatch stays dry."""
        s = self.pet_solver()
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.step()
        s1 = s.subcatchments["S1"]
        s2 = s.subcatchments["S2"]
        # Storm is active, so ponded depth >> PET rate: loss equals the rate.
        self.assertAlmostEqual(s1.evap, _PET, delta=_PET * 1e-3)
        # Climate evap is 0.0, so the unforced subcatchment shows none.
        self.assertAlmostEqual(s2.evap, 0.0, delta=1e-12)

    def test_add_mode(self):
        """Case 5: ADD augments the climate rate (0.0 here)."""
        s = self.pet_solver()
        _spinup(s)
        s.forcing.subcatchment_evap(
            "S1", 1.5, mode=ForcingMode.ADD, persist=True)
        s.step()
        self.assertAlmostEqual(s.subcatchments["S1"].evap, 1.5, delta=1.5 * 1e-3)

    def test_one_shot_resets_after_one_step(self):
        """Case 6: persist=False affects exactly one step."""
        s = self.pet_solver()
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=False)
        s.step()
        self.assertAlmostEqual(s.subcatchments["S1"].evap, _PET, delta=_PET * 1e-3)
        s.step()
        self.assertAlmostEqual(s.subcatchments["S1"].evap, 0.0, delta=1e-12)

    def test_clear_reverts_to_climate(self):
        """Case 4: clearing reverts to the climate-derived rate (0.0)."""
        s = self.pet_solver()
        _spinup(s)
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.step()
        self.assertGreater(s.subcatchments["S1"].evap, 0.0)
        s.forcing.clear_all()
        s.step()
        self.assertAlmostEqual(s.subcatchments["S1"].evap, 0.0, delta=1e-12)


class TestCappingAndMassBalance(PetSolverCase):
    def test_capping_to_available_water(self):
        """Case 2: an extreme PET cannot evaporate more than is available."""
        s = self.pet_solver()
        extreme = 1.0e6  # in/day — far beyond any plausible ponded volume
        s.forcing.subcatchment_evap("S1", extreme, persist=True)
        for _ in range(_SPINUP_STEPS):
            s.step()
        s1 = s.subcatchments["S1"]
        # Loss is capped by available water: strictly below the demand,
        # and never negative.
        self.assertGreaterEqual(s1.evap, 0.0)
        self.assertLess(s1.evap, extreme)

    def test_mass_balance_continuity(self):
        """Case 3: persistent prescription keeps runoff continuity sound."""
        s = self.pet_solver()
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        s.forcing.subcatchment_evap("S2", _PET, persist=True)
        while s.step():
            pass
        s.end()
        err = s.mass_balance.runoff_continuity_error
        self.assertLess(abs(err), 0.5)

    def test_evap_appears_in_runoff_totals(self):
        """Case 3 (totals): forced evaporation shows up in the system totals."""
        from openswmm.engine import RunoffTotal
        s = self.pet_solver()
        s.forcing.subcatchment_evap("S1", _PET, persist=True)
        for _ in range(2 * _SPINUP_STEPS):
            s.step()
        total_evap = s.mass_balance.runoff_total(RunoffTotal.EVAP)
        self.assertGreater(total_evap, 0.0)


class TestClimateRateGetter(PetSolverCase):
    def test_getter_matches_model_climate(self):
        """L9/R8: the model's constant evap is 0.0 in/day."""
        s = self.pet_solver()
        _spinup(s)
        self.assertAlmostEqual(s.forcing.climate_evap_rate(), 0.0, delta=1e-12)

    def test_adjustment_composition_round_trip(self):
        """Case 13: read rate, apply caller-side adjustment, prescribe result.

        The engine applies the composed rate verbatim — no further
        engine-side adjustment.
        """
        s = self.pet_solver()
        _spinup(s)
        base = s.forcing.climate_evap_rate()  # 0.0 for this model
        composed = base * 0.8 + 1.2           # caller-side logic
        s.forcing.subcatchment_evap("S1", composed, persist=True)
        s.step()
        self.assertAlmostEqual(
            s.subcatchments["S1"].evap, composed, delta=abs(composed) * 1e-3)


def _derived_model(name, replacements=()):
    """Write a modified copy of the site model to the reviewable output dir.

    Always applies _DETERMINISTIC_RUNOFF so the runoff clock fires exactly once
    per routing step, then the caller's extra replacements.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_INP) as f:
        text = f.read()
    for old, new in (*_DETERMINISTIC_RUNOFF, *replacements):
        assert old in text, f"expected {old!r} in template model"
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


class TestDryOnlyBypass(PetSolverCase):
    def test_prescription_bypasses_dry_only(self):
        """Case 7: prescribed PET evaporates even when DRY_ONLY suppresses
        the climate rate during rainfall."""
        inp = _derived_model("pet_dry_only.inp", [
            ("CONSTANT         0.0", "CONSTANT         5.0"),
            ("DRY_ONLY         NO", "DRY_ONLY         YES"),
        ])
        base = os.path.join(_OUT_DIR, "pet_dry_only")
        s = Solver(inp, base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        try:
            _spinup(s)  # storm is active → DRY_ONLY zeroes climate evap
            s.step()
            self.assertAlmostEqual(s.subcatchments["S2"].evap, 0.0, delta=1e-12)
            s.forcing.subcatchment_evap("S1", _PET, persist=True)
            s.step()
            self.assertAlmostEqual(s.subcatchments["S1"].evap, _PET, delta=_PET * 1e-3)
        finally:
            s.close()
            s.destroy()


class TestSIUnits(PetSolverCase):
    def test_si_prescription_round_trips_in_mm_day(self):
        """Case 9: on an SI model the API accepts and reports mm/day."""
        inp = _derived_model("pet_si_units.inp", [
            ("FLOW_UNITS           CFS", "FLOW_UNITS           CMS"),
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
            self.assertAlmostEqual(
                s.subcatchments["S1"].evap, pet_mm_day,
                delta=pet_mm_day * 1e-3)
        finally:
            s.close()
            s.destroy()


class TestErrorPaths(PetSolverCase):
    def test_bad_subcatchment_raises(self):
        """Case 12: unknown ids and bad indices are rejected."""
        s = self.pet_solver()
        with self.assertRaises(Exception):
            s.forcing.subcatchment_evap("NO_SUCH_SUBCATCH", _PET)

    def test_bad_mode_raises(self):
        s = self.pet_solver()
        with self.assertRaises(Exception):
            s.forcing.subcatchment_evap("S1", _PET, mode=99)
