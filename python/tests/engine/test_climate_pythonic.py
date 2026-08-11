"""Climate wrapper coverage (gap-review 2026-07-06).

The ``Climate`` OOP wrapper (``solver.climate``) was never exercised via its
own property surface — climate had only been reached through the forcing /
legacy paths. Round-trips the numeric scalar and monthly / ADC array
properties on the standard site-drainage model (defaults are readable /
writable even without an explicit ``[TEMPERATURE]`` section).
"""
from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._climate  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import Climate, Solver

from tests._paths import artifact_dir

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "data", "solver", "site_drainage_example.inp")


class ClimateCase(unittest.TestCase):
    """Base class providing the opened-solver helper (former fixture)."""

    def clim_solver(self):
        d = artifact_dir(self)
        s = Solver(_MODEL, os.path.join(d, "cl.rpt"), os.path.join(d, "cl.out"))
        s.open()
        self.addCleanup(self._close_destroy, s)
        return s

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestClimateProperty(ClimateCase):
    def test_type(self):
        clim_solver = self.clim_solver()
        self.assertIsInstance(clim_solver.climate, Climate)

    def test_enum_ints_readable(self):
        clim_solver = self.clim_solver()
        for attr in ["temp_source", "evap_type", "wind_type", "temp_units"]:
            with self.subTest(attr=attr):
                self.assertIsInstance(getattr(clim_solver.climate, attr), int)


class TestClimateScalarRoundTrip(ClimateCase):
    def test_scalar_roundtrip(self):
        clim_solver = self.clim_solver()
        for attr, value in [
            ("elevation", 123.4),
            ("latitude", 41.5),
            ("longitude_correction", -5.0),
            ("snow_temp", 33.0),
            ("ati_weight", 0.5),
            ("neg_melt_ratio", 0.6),
        ]:
            with self.subTest(attr=attr, value=value):
                setattr(clim_solver.climate, attr, value)
                self.assertAlmostEqual(
                    getattr(clim_solver.climate, attr), value, places=6)


class TestClimateArrayRoundTrip(ClimateCase):
    def test_array_roundtrip(self):
        clim_solver = self.clim_solver()
        for attr, n in [
            ("evap_monthly", 12),
            ("pan_coeff", 12),
            ("wind_monthly", 12),
            ("adjust_temperature", 12),
            ("adjust_evaporation", 12),
            ("adjust_rainfall", 12),
            ("adjust_conductivity", 12),
            ("adc_impervious", 10),
            ("adc_pervious", 10),
        ]:
            with self.subTest(attr=attr, n=n):
                # Non-zero, strictly-increasing fractional values in (0, 1): valid for
                # ADC curves and faithful for the monthly/adjust arrays. A zero base is
                # avoided because the conductivity multiplier treats 0 as the "no
                # adjustment" sentinel and reads back as 1.0.
                vals = [round(0.5 + 0.01 * i, 3) for i in range(n)]
                setattr(clim_solver.climate, attr, vals)
                got = list(getattr(clim_solver.climate, attr))
                self.assertEqual(len(got), len(vals))
                for g, v in zip(got, vals):
                    self.assertAlmostEqual(g, v, places=6)
