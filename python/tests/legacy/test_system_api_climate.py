# Description: System-level climate prescription tests (legacy engine).
# Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md items M1 (air temperature)
# and M2 (wind speed): swmm_API_TEMPERATURE / swmm_API_WINDSPEED setters,
# the read-only swmm_TEMPERATURE / swmm_WINDSPEED getters, clearing, and
# SI unit conversion.
#
# All tests run against the real legacy solver (no mocks). Report/output
# files are written to tests/legacy/output so they remain reviewable.
#
# Created on: 2026-06-11

import os
import unittest

from tests.data import solver as example_solver_data
from openswmm import solver
from openswmm.legacy.engine import LegacySystem

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")


def _make_solver(name, inp=None):
    os.makedirs(_OUT_DIR, exist_ok=True)
    inp = inp or example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
    base = os.path.join(_OUT_DIR, "climate_" + name)
    return solver.Solver(
        inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")


def _derived_model(name, replacements):
    """Write a modified copy of the site model to the reviewable output dir."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    src = example_solver_data.SITE_DRAINAGE_EXAMPLE_INPUT_FILE
    with open(src) as f:
        text = f.read()
    for old, new in replacements:
        assert old in text
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


class TestApiTemperature(unittest.TestCase):

    def test_set_get_clear_round_trip(self):
        s = _make_solver("temp_roundtrip")
        s.initialize()
        sys = LegacySystem(s)
        s.step()
        sys.set_api_temperature(50.0)
        self.assertAlmostEqual(sys.get_api_temperature(), 50.0, delta=1e-9)
        s.step()  # climate_setState applies the prescription
        self.assertAlmostEqual(sys.get_temperature(), 50.0, delta=1e-9)
        sys.clear_api_temperature()
        self.assertLessEqual(sys.get_api_temperature(), -999.0)
        s.finalize()

    def test_negative_temperatures_are_valid(self):
        # Sub-freezing prescriptions must not be mistaken for "clear".
        s = _make_solver("temp_negative")
        s.initialize()
        sys = LegacySystem(s)
        s.step()
        sys.set_api_temperature(-10.0)
        s.step()
        self.assertAlmostEqual(sys.get_temperature(), -10.0, delta=1e-9)
        s.finalize()

    def test_si_units_round_trip(self):
        # On an SI model the API accepts and reports deg C.
        inp = _derived_model("climate_si_units.inp", [
            ("FLOW_UNITS           CFS", "FLOW_UNITS           CMS"),
        ])
        s = _make_solver("temp_si", inp=inp)
        s.initialize()
        sys = LegacySystem(s)
        s.step()
        sys.set_api_temperature(10.0)  # deg C
        self.assertAlmostEqual(sys.get_api_temperature(), 10.0, delta=1e-9)
        s.step()
        self.assertAlmostEqual(sys.get_temperature(), 10.0, delta=1e-9)
        s.finalize()


class TestApiWindSpeed(unittest.TestCase):

    def test_set_get_clear_round_trip(self):
        s = _make_solver("wind_roundtrip")
        s.initialize()
        sys = LegacySystem(s)
        s.step()
        # Site model has no wind data -> default 0; prescription wins.
        sys.set_api_wind_speed(12.5)
        self.assertAlmostEqual(sys.get_api_wind_speed(), 12.5, delta=1e-9)
        s.step()
        self.assertAlmostEqual(sys.get_wind_speed(), 12.5, delta=1e-9)
        sys.clear_api_wind_speed()
        self.assertLess(sys.get_api_wind_speed(), 0.0)
        s.step()
        self.assertAlmostEqual(sys.get_wind_speed(), 0.0, delta=1e-9)
        s.finalize()


if __name__ == "__main__":
    unittest.main()
