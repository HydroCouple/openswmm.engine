"""Climate forcing tests (refactored engine) — plan rows 1–3.

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md items:
  * M1 — system-wide air temperature forcing (REPLACE/ADD/persist/clear)
  * M2 — system-wide wind speed forcing
  * A2 — regression: subcatchment rainfall forcing must survive the
    per-step gage re-read in the runoff solver
  * A5 — regression: ``Forcing.clear`` must clear the channels of the
    requested object kind (it previously passed object-kind codes where
    channel codes were expected)

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). The aligned base model is written to ``tests/engine/output``
and per-test report/output files to ``tests/_artifacts`` so they remain
reviewable after the run.

The bundled site-drainage model has no temperature or wind data source,
so the engine defaults apply (70 deg F, 0 mph) and any other value
observed is attributable to the forcing alone. Snowmelt-response tests
require a snowpack fixture and are tracked as follow-up work in the plan.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver, ForcingMode, ForcingTarget

from tests._paths import artifact_dir

_DATA_DIR = os.path.join(
    os.path.dirname(os.path.dirname(__file__)), "data", "solver")
_INP = os.path.join(_DATA_DIR, "site_drainage_example.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

_DEFAULT_TEMP_F = 70.0   # ClimateState default when no source is present
_DEFAULT_WIND = 0.0

# Climate state (temperature/wind/evap) is refreshed on the runoff clock, not
# every routing step. The bundled model runs WET_STEP 1 min / DRY_STEP 1 hr
# over a 15 s ROUTING_STEP, so a single-step assertion after a prescription
# would not see a runoff fire. Align both the wet and dry runoff steps to the
# routing step so each step() triggers exactly one runoff (and climate) update.
_WET_ALIGN = ("WET_STEP             00:01:00", "WET_STEP             00:00:15")
_DRY_ALIGN = ("DRY_STEP             01:00:00", "DRY_STEP             00:00:15")


def _aligned_model():
    """Write a copy of the site model with runoff steps aligned to routing."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_INP) as f:
        text = f.read()
    for old, new in (_WET_ALIGN, _DRY_ALIGN):
        assert old in text, f"expected {old!r} in template model"
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, "climate_base.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class ClimateSolverCase(unittest.TestCase):
    """Base TestCase providing a running climate-aligned Solver."""

    def climate_solver(self):
        """A running Solver whose rpt/out files land in a reviewable folder."""
        base = os.path.join(artifact_dir(self), "climate")
        s = Solver(_aligned_model(), base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        self.addCleanup(self._teardown_solver, s)
        return s

    @staticmethod
    def _teardown_solver(s):
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


class TestTemperatureForcing(ClimateSolverCase):
    def test_replace_round_trip(self):
        s = self.climate_solver()
        s.forcing.climate_temperature(50.0, persist=True)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_temperature(), 50.0,
                               places=6)

    def test_add_mode_is_delta(self):
        s = self.climate_solver()
        s.forcing.climate_temperature(
            5.0, mode=ForcingMode.ADD, persist=True)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_temperature(),
                               _DEFAULT_TEMP_F + 5.0, places=6)

    def test_one_shot_resets(self):
        s = self.climate_solver()
        s.forcing.climate_temperature(50.0, persist=False)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_temperature(), 50.0,
                               places=6)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_temperature(),
                               _DEFAULT_TEMP_F, places=6)

    def test_clear_climate_target(self):
        s = self.climate_solver()
        s.forcing.climate_temperature(50.0, persist=True)
        s.step()
        s.forcing.clear(ForcingTarget.CLIMATE, 0)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_temperature(),
                               _DEFAULT_TEMP_F, places=6)


class TestWindForcing(ClimateSolverCase):
    def test_replace_round_trip(self):
        s = self.climate_solver()
        s.forcing.climate_wind(12.5, persist=True)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_wind_speed(), 12.5,
                               places=6)

    def test_negative_override_rejected(self):
        s = self.climate_solver()
        with self.assertRaises(Exception):
            s.forcing.climate_wind(-1.0)

    def test_one_shot_resets(self):
        s = self.climate_solver()
        s.forcing.climate_wind(8.0, persist=False)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_wind_speed(), 8.0,
                               places=6)
        s.step()
        self.assertAlmostEqual(s.forcing.get_climate_wind_speed(),
                               _DEFAULT_WIND, places=6)


class TestRainfallForcingRegression(ClimateSolverCase):
    """A2: rainfall forcing must not be clobbered by the gage re-read."""

    def test_override_reaches_subcatchment(self):
        s = self.climate_solver()
        s.forcing.subcatchment_rainfall("S1", 2.0, persist=True)
        s.step()
        # rainfall getter returns user units (in/hr); the storm value at
        # the gage differs from 2.0, so equality proves the override won.
        self.assertAlmostEqual(s.subcatchments["S1"].rainfall, 2.0,
                               delta=2.0 * 1e-6)

    def test_add_mode_augments_gage(self):
        s = self.climate_solver()
        s.forcing.subcatchment_rainfall(
            "S1", 1.0, mode=ForcingMode.ADD, persist=True)
        s.step()
        # S1 and S2 share the same gage, so S1 must read exactly the
        # gage value (visible on S2) plus the added 1.0 in/hr.
        expected = s.subcatchments["S2"].rainfall + 1.0
        self.assertAlmostEqual(s.subcatchments["S1"].rainfall, expected,
                               delta=abs(expected) * 1e-6)


class TestClearTargetRegression(ClimateSolverCase):
    """A5: clear() must clear the requested object kind's channels."""

    def test_clear_subcatch_clears_subcatch_channels(self):
        s = self.climate_solver()
        s.forcing.subcatchment_rainfall("S1", 2.0, persist=True)
        s.forcing.subcatchment_evap("S1", 2.4, persist=True)
        s.step()
        self.assertAlmostEqual(s.subcatchments["S1"].rainfall, 2.0,
                               delta=2.0 * 1e-6)
        s.forcing.clear(ForcingTarget.SUBCATCH, "S1")
        s.step()
        # Both subcatchment channels are cleared: rainfall reverts to the
        # gage and evap to the (zero) climate rate.
        expected = s.subcatchments["S2"].rainfall
        self.assertAlmostEqual(s.subcatchments["S1"].rainfall, expected,
                               delta=max(abs(expected) * 1e-6, 1e-12))
        self.assertAlmostEqual(s.subcatchments["S1"].evap, 0.0, delta=1e-12)

    def test_clear_subcatch_leaves_other_targets(self):
        s = self.climate_solver()
        s.forcing.climate_temperature(50.0, persist=True)
        s.forcing.subcatchment_rainfall("S1", 2.0, persist=True)
        s.step()
        s.forcing.clear(ForcingTarget.SUBCATCH, "S1")
        s.step()
        # The climate prescription must survive a subcatchment clear.
        self.assertAlmostEqual(s.forcing.get_climate_temperature(), 50.0,
                               places=6)
