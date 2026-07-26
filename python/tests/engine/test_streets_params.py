"""New-API gap closure — Streets.get_params (swmm_street_get_params).

Round-trips set_params -> get_params against the real engine (no mocks),
covering the newly-added inverse street-parameter accessor.
"""

from __future__ import annotations

import unittest

try:
    import openswmm.engine._infrastructure  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests.engine._solver_cases import EngineSolverCase


class TestStreetGetParams(EngineSolverCase):
    def test_set_then_get_round_trip(self):
        solver = self.opened_solver()
        streets = solver.infrastructure.streets
        idx = streets.add("ST_TEST")
        streets.set_params(
            idx,
            t_crown=0.5, h_curb=0.6, sx=0.04, n_road=0.016,
            gutter_depres=2.0, gutter_width=2.0, sides=2,
            back_width=10.0, back_slope=0.02, back_n=0.02,
        )
        p = streets.get_params(idx)
        self.assertAlmostEqual(p["t_crown"], 0.5, places=6)
        self.assertAlmostEqual(p["h_curb"], 0.6, places=6)
        self.assertAlmostEqual(p["sx"], 0.04, places=6)
        self.assertAlmostEqual(p["n_road"], 0.016, places=6)
        self.assertEqual(p["sides"], 2)
        self.assertAlmostEqual(p["back_width"], 10.0, places=6)
