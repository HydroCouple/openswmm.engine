# Description: Snow prescription tests (legacy engine).
# Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md item M3's legacy side:
# api snowfall now ACCUMULATES in the snow pack (snow_plowSnow includes
# Subcatch.apiSnowfall; previously prescribed snow only influenced melt
# computations, leaving a runoff continuity hole), and the prescribed
# air temperature (M1) drives melt on/off.
#
# Uses the checked-in site_drainage_snow.inp fixture (snow pack SP1 on
# S1 only; constant 25 deg F temperature series; dividing temp 34 F).
# Runoff totals (final snow cover) are only valid after end(), so each
# scenario is a complete run with a mid-run prescription schedule.
#
# All tests run against the real legacy solver (no mocks). Report/output
# files are written to tests/legacy/output so they remain reviewable.
#
# Created on: 2026-06-11

import os
import unittest

from tests.data import solver as example_solver_data
from openswmm import solver
from openswmm.legacy.engine import LegacySubcatchments, LegacySystem

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")
# Fire the mid-run schedule switch 1 simulated hour in. The legacy solver
# advances on a variable clock (WET_STEP while the storm runs, DRY_STEP after,
# subject to VARIABLE_STEP), so the number of step() calls is not fixed — key
# the switch off simulated time, not a step count.
from datetime import timedelta as _timedelta
_SWITCH_AFTER = _timedelta(hours=1)


def _run_scenario(name, before=None, after=None):
    """Run the snow fixture to completion.

    ``before(subs, sys)`` applies prescriptions at the first step;
    ``after(subs, sys)`` applies prescriptions once 1 simulated hour has
    elapsed. Returns the post-run runoff totals dict.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    inp = example_solver_data.SITE_DRAINAGE_SNOW_INPUT_FILE
    base = os.path.join(_OUT_DIR, "snow_" + name)
    s = solver.Solver(
        inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")
    s.initialize()
    subs = LegacySubcatchments(s)
    sys = LegacySystem(s)
    if before is not None:
        before(subs, sys)
    start = s.current_datetime
    switched = False
    while s.solver_state != solver.SolverState.FINISHED:
        s.step()
        if (not switched and after is not None
                and s.current_datetime - start >= _SWITCH_AFTER):
            after(subs, sys)
            switched = True
    s.end()
    totals = sys.runoff_totals
    errors = sys.mass_balance_error
    s.finalize()
    return totals, errors


class TestApiSnowfallAccumulation(unittest.TestCase):

    def test_gage_snow_accumulates(self):
        # At 25 F (< 34 F dividing temp) the storm arrives as snow and
        # must persist to the end of the cold simulation.
        totals, _ = _run_scenario("gage_accum")
        self.assertGreater(totals["final_snow_cover"], 0.0)

    def test_api_snowfall_accumulates(self):
        # Prescribed snowfall must grow the pack even when the air is too
        # warm for gage snow (regression: apiSnowfall previously never
        # accumulated). 50 F air melts at <= 0.09 in/hr while 2 in/hr is
        # prescribed all run, so cover must remain at the end.
        def before(subs, sys):
            sys.set_api_temperature(50.0)   # gage precip becomes rain
            subs["S1"].set_api_snowfall(2.0)
        totals, _ = _run_scenario("api_accum", before=before)
        self.assertGreater(totals["final_snow_cover"], 0.0)

    def test_api_temperature_drives_melt(self):
        # Same snowfall schedule for the first hour; run A stays cold
        # afterwards, run B forces a strong warm-up. B must end with
        # less snow cover than A.
        def before(subs, sys):
            subs["S1"].set_api_snowfall(3.0)

        def stop_snow_cold(subs, sys):
            subs["S1"].set_api_snowfall(0.0)
            sys.set_api_temperature(10.0)   # below tbase: no melt

        def stop_snow_warm(subs, sys):
            subs["S1"].set_api_snowfall(0.0)
            sys.set_api_temperature(80.0)   # strong melt

        totals_cold, _ = _run_scenario(
            "melt_cold", before=before, after=stop_snow_cold)
        totals_warm, _ = _run_scenario(
            "melt_warm", before=before, after=stop_snow_warm)
        self.assertGreater(totals_cold["final_snow_cover"], 0.0)
        self.assertLess(totals_warm["final_snow_cover"],
                        totals_cold["final_snow_cover"])

    def test_runoff_continuity_with_prescription(self):
        def before(subs, sys):
            subs["S1"].set_api_snowfall(2.0)
        _, errors = _run_scenario("continuity", before=before)
        runoff_err = errors[0]
        self.assertLess(abs(runoff_err), 0.5)


if __name__ == "__main__":
    unittest.main()
