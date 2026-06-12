# Description: Subcatchment PET prescription tests (legacy engine).
# Covers docs/SUBCATCHMENT_PET_PRESCRIPTION_PLAN.md section 6 for the legacy
# API: swmm_SUBCATCH_API_PET set/get/clear, mass balance via runoff totals,
# the swmm_EVAPRATE system getter, and adjustment composition.
#
# All tests run against the real legacy solver (no mocks). Report/output
# files are written to tests/legacy/output so they remain reviewable.
#
# Created on: 2026-06-10

import os
import unittest

from tests.data import solver as example_solver_data
from openswmm import solver
from openswmm.legacy.engine import (
    LegacySubcatchments,
    LegacySystem,
    ExternalForcingLog,
)

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

# The site-drainage model uses CONSTANT evaporation of 0.0 in/day
# (DRY_ONLY NO, US units) and a 2-yr design storm beginning at sim start,
# so any non-zero evaporation observed below comes from the prescribed
# PET alone.
_PET = 2.4  # in/day
_SPINUP_STEPS = 30

# Make the runoff clock deterministic so a prescription is observable on the
# next step and the sampled evaporation is stable: fix the routing step
# (VARIABLE_STEP 0) and align WET_STEP to it.
_DETERMINISTIC = (
    ("WET_STEP             00:01:00", "WET_STEP             00:00:15"),
    ("VARIABLE_STEP        0.75", "VARIABLE_STEP        0.0"),
)

# A prescribed PET cannot evaporate from the zero-depression impervious
# subarea (it sheds water immediately and holds none), so the area-weighted
# subcatchment evaporation is at most the PET rate and typically a little
# below it. Assertions below check the prescription drives a substantial,
# physically-valid evaporation (in the open interval up to PET) rather than
# demanding exactly the full rate over the whole area.
_WETTED_FRACTION_FLOOR = 0.7


def _deterministic_model(name, replacements=()):
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE) as f:
        text = f.read()
    for old, new in (*_DETERMINISTIC, *replacements):
        assert old in text, f"expected {old!r} in template model"
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def _make_solver(name):
    inp = _deterministic_model("pet_" + name + ".inp")
    base = os.path.join(_OUT_DIR, "pet_" + name)
    return solver.Solver(
        inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")


class TestApiPetPrescription(unittest.TestCase):

    def test_override_takes_effect(self):
        # Case 1: prescribed PET drives evap loss; untouched subcatch stays dry.
        s = _make_solver("override")
        s.initialize()
        subs = LegacySubcatchments(s)
        for _ in range(_SPINUP_STEPS):
            s.step()
        subs["S1"].set_api_pet(_PET)
        s.step()
        # The prescription drives a substantial PET-limited evaporation,
        # bounded above by the rate (zero-depression impervious area holds
        # no water to evaporate).
        self.assertGreater(subs["S1"].evaporation, _WETTED_FRACTION_FLOOR * _PET)
        self.assertLessEqual(subs["S1"].evaporation, _PET * 1.01)
        # Climate evap is 0.0, so the unforced subcatchment shows none.
        self.assertAlmostEqual(subs["S2"].evaporation, 0.0, delta=1e-12)
        s.finalize()

    def test_getter_round_trip(self):
        # Case 11: set/get round trip in user units; negative when cleared.
        s = _make_solver("roundtrip")
        s.initialize()
        subs = LegacySubcatchments(s)
        s.step()
        subs["S1"].set_api_pet(_PET)
        self.assertAlmostEqual(subs["S1"].get_api_pet(), _PET, delta=1e-9)
        subs["S1"].clear_api_pet()
        self.assertLess(subs["S1"].get_api_pet(), 0.0)
        s.finalize()

    def test_clear_reverts_to_climate(self):
        # Case 4: clearing reverts to climate-derived evaporation (0.0 here).
        s = _make_solver("clear")
        s.initialize()
        subs = LegacySubcatchments(s)
        for _ in range(_SPINUP_STEPS):
            s.step()
        subs["S1"].set_api_pet(_PET)
        s.step()
        self.assertGreater(subs["S1"].evaporation, 0.0)
        subs["S1"].clear_api_pet()
        s.step()
        self.assertAlmostEqual(subs["S1"].evaporation, 0.0, delta=1e-12)
        s.finalize()

    def test_persists_across_steps(self):
        # Legacy prescription is sticky until cleared (apiRainfall pattern).
        s = _make_solver("persist")
        s.initialize()
        subs = LegacySubcatchments(s)
        for _ in range(_SPINUP_STEPS):
            s.step()
        subs["S1"].set_api_pet(_PET)
        for _ in range(3):
            s.step()
            self.assertGreater(subs["S1"].evaporation, 0.0)
        s.finalize()

    def test_capping_to_available_water(self):
        # Case 2: an extreme PET cannot evaporate more than is available.
        s = _make_solver("capping")
        s.initialize()
        subs = LegacySubcatchments(s)
        extreme = 1.0e6  # in/day
        subs["S1"].set_api_pet(extreme)
        for _ in range(_SPINUP_STEPS):
            s.step()
        evap = subs["S1"].evaporation
        self.assertGreaterEqual(evap, 0.0)
        self.assertLess(evap, extreme)
        s.finalize()

    def test_mass_balance_continuity(self):
        # Case 3: persistent prescription keeps runoff continuity sound and
        # the forced losses appear in the system runoff totals.
        s = _make_solver("massbal")
        s.initialize()
        subs = LegacySubcatchments(s)
        subs["S1"].set_api_pet(_PET)
        subs["S2"].set_api_pet(_PET)
        while s.solver_state != solver.SolverState.FINISHED:
            s.step()
        sys = LegacySystem(s)
        totals = sys.runoff_totals
        self.assertGreater(totals["evap"], 0.0)
        runoff_err, _, _ = sys.mass_balance_error
        self.assertLess(abs(runoff_err), 0.5)
        s.finalize()

    def test_forcing_log_records_prescription(self):
        # The audit log captures the prescription like other API setters.
        s = _make_solver("log")
        s.initialize()
        subs = LegacySubcatchments(s)
        log = ExternalForcingLog()
        s.step()
        subs["S1"].set_api_pet(_PET, log=log)
        records = log.records
        self.assertEqual(records[-1]["property"], "api_pet")
        self.assertEqual(
            records[-1]["mass_balance_category"], "runoff.evaporation")
        s.finalize()


def _derived_model(name, replacements):
    """Write a modified copy of the site model to the reviewable output dir."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    inp = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
    with open(inp) as f:
        text = f.read()
    for old, new in replacements:
        assert old in text
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


class TestDryOnlyBypass(unittest.TestCase):

    def test_prescription_bypasses_dry_only(self):
        # Case 7: prescribed PET evaporates even when DRY_ONLY suppresses
        # the climate rate during rainfall.
        inp = _deterministic_model("pet_dry_only.inp", [
            ("CONSTANT         0.0", "CONSTANT         5.0"),
            ("DRY_ONLY         NO", "DRY_ONLY         YES"),
        ])
        base = os.path.join(_OUT_DIR, "pet_dry_only")
        s = solver.Solver(
            inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")
        s.initialize()
        subs = LegacySubcatchments(s)
        # Advance to a step where it is actively raining: DRY_ONLY suppresses
        # the climate evap only while it rains, and the design storm rains in
        # its first ~30 minutes, so check early (the report step is coarse).
        raining_step = False
        for _ in range(20):
            s.step()
            if subs["S2"].rainfall > 0.0:
                raining_step = True
                break
        self.assertTrue(raining_step, "expected an actively raining step")
        # Climate evap (5 in/day) is fully suppressed by DRY_ONLY during rain.
        self.assertAlmostEqual(subs["S2"].evaporation, 0.0, delta=1e-12)
        # A prescribed PET on S1 bypasses DRY_ONLY and evaporates despite the
        # rainfall (PET-limited, so a little below the full rate).
        subs["S1"].set_api_pet(_PET)
        s.step()
        self.assertGreater(subs["S1"].evaporation, _WETTED_FRACTION_FLOOR * _PET)
        self.assertLessEqual(subs["S1"].evaporation, _PET * 1.01)
        s.finalize()


class TestSIUnits(unittest.TestCase):

    def test_si_prescription_round_trips_in_mm_day(self):
        # Case 9: on an SI model the API accepts and reports mm/day.
        inp = _derived_model("pet_si_units.inp", [
            ("FLOW_UNITS           CFS", "FLOW_UNITS           CMS"),
        ])
        base = os.path.join(_OUT_DIR, "pet_si_units")
        pet_mm_day = 5.0
        s = solver.Solver(
            inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")
        s.initialize()
        subs = LegacySubcatchments(s)
        for _ in range(_SPINUP_STEPS):
            s.step()
        subs["S1"].set_api_pet(pet_mm_day)
        s.step()
        self.assertAlmostEqual(
            subs["S1"].evaporation, pet_mm_day, delta=pet_mm_day * 1e-2)
        # Getter round-trips in the same units.
        self.assertAlmostEqual(
            subs["S1"].get_api_pet(), pet_mm_day, delta=1e-9)
        s.finalize()


class TestSystemEvapRateGetter(unittest.TestCase):

    def test_getter_matches_model_climate(self):
        # L9: the model's constant evaporation is 0.0 in/day.
        s = _make_solver("sysgetter")
        s.initialize()
        s.step()
        sys = LegacySystem(s)
        self.assertAlmostEqual(sys.get_evap_rate(), 0.0, delta=1e-12)
        s.finalize()

    def test_adjustment_composition_round_trip(self):
        # Case 13: read rate, apply caller-side adjustment, prescribe result.
        s = _make_solver("compose")
        s.initialize()
        subs = LegacySubcatchments(s)
        sys = LegacySystem(s)
        for _ in range(_SPINUP_STEPS):
            s.step()
        composed = sys.get_evap_rate() * 0.8 + 1.2  # caller-side logic
        subs["S1"].set_api_pet(composed)
        s.step()
        # The composed rate is applied verbatim as the PET demand; observed
        # evaporation is PET-limited (a little below the rate because the
        # zero-depression impervious subarea holds no water).
        self.assertGreater(
            subs["S1"].evaporation, _WETTED_FRACTION_FLOOR * composed)
        self.assertLessEqual(subs["S1"].evaporation, composed * 1.01)
        s.finalize()


if __name__ == "__main__":
    unittest.main()
