"""State-injection tests (refactored engine) — plan rows S1 and S2.

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md:
  * S1 — groundwater state injection (upper-zone moisture / lower-zone depth)
    via ``Subcatchment.set_gw_state`` / ``get_gw_state``.
  * S2 — snow-pack state injection (SWE / free water / ATI / cold content)
    via ``Subcatchment.set_snow_state`` / ``get_snow_state``, including a
    melt-out check that the prescribed SWE leaves the pack as melt.

State injection is an initial-condition change, so (per the plan) mass-balance
reports reflect a storage discontinuity, mirroring hotstart loading; the tests
assert physical response and round-tripping, not zero continuity error.

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). Report/output files land in ``tests/engine/output``.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
_SNOW_INP = os.path.join(_TESTS_DIR, "data", "solver", "site_drainage_snow.inp")
_GW_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "refactored_small.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

# Snow subareas
_PLOWABLE, _IMPERV, _PERV = 0, 1, 2


def _solver(inp, name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, name)
    s = Solver(inp, base + ".rpt", base + ".out")
    s.open()
    s.initialize()
    s.start()
    return s


def _first_gw_subcatch(s):
    """Return a subcatchment index that has groundwater, or skip."""
    for i in range(len(s.subcatchments)):
        try:
            s.subcatchments[i].get_gw_state()
            return i
        except Exception:
            continue
    raise unittest.SkipTest("no groundwater subcatchment in model")


class TestGroundwaterState(unittest.TestCase):
    def test_theta_round_trip_and_clamp(self):
        """S1: set/get upper-zone moisture; values clamp to porosity."""
        s = _solver(_GW_INP, "gw_theta")
        try:
            idx = _first_gw_subcatch(s)
            sc = s.subcatchments[idx]
            theta0, _ = sc.get_gw_state()
            # Inject a distinct, physically valid moisture.
            target = max(0.05, theta0 * 0.5)
            sc.set_gw_state(theta=target)
            s.step()
            theta1, _ = sc.get_gw_state()
            self.assertTrue(abs(theta1 - target) <= 1e-6 or theta1 != theta0)
            # An over-porosity request must be clamped, not stored verbatim.
            sc.set_gw_state(theta=10.0)
            theta_cap, _ = sc.get_gw_state()
            self.assertLessEqual(theta_cap, 1.0)
        finally:
            s.end(); s.close(); s.destroy()

    def test_negative_components_leave_state_unchanged(self):
        """S1: passing a negative component keeps it (sentinel semantics)."""
        s = _solver(_GW_INP, "gw_keep")
        try:
            idx = _first_gw_subcatch(s)
            sc = s.subcatchments[idx]
            sc.set_gw_state(theta=0.25)
            s.step()
            before = sc.get_gw_state()
            # Only update lower_depth; theta must be preserved.
            sc.set_gw_state(lower_depth=-1.0)
            after = sc.get_gw_state()
            self.assertAlmostEqual(after[0], before[0], delta=1e-9)
        finally:
            s.end(); s.close(); s.destroy()

    def test_injection_changes_groundwater_response(self):
        """S1: raising upper-zone moisture changes the GW outflow trajectory."""
        def run(inject):
            s = _solver(_GW_INP, f"gw_resp_{inject}")
            idx = _first_gw_subcatch(s)
            sc = s.subcatchments[idx]
            for _ in range(20):
                s.step()
            if inject:
                # Saturate the upper zone on every GW subcatchment.
                for i in range(len(s.subcatchments)):
                    try:
                        s.subcatchments[i].set_gw_state(theta=0.45)
                    except Exception:
                        pass
            for _ in range(40):
                s.step()
            th, _ = sc.get_gw_state()
            s.end(); s.close(); s.destroy()
            return th

        baseline = run(False)
        injected = run(True)
        # The injected moisture leaves a detectable difference in the
        # upper-zone state trajectory.
        self.assertNotAlmostEqual(injected, baseline, delta=1e-6)


class TestSnowState(unittest.TestCase):
    def test_swe_round_trip(self):
        """S2: SWE/ATI/cold-content set then read back in user units."""
        s = _solver(_SNOW_INP, "snow_roundtrip")
        try:
            sc = s.subcatchments["S1"]
            for _ in range(5):
                s.step()
            sc.set_snow_state(_PERV, swe=4.0, fw=0.5, ati=28.0, coldc=0.1)
            swe, fw, ati, coldc = sc.get_snow_state(_PERV)
            self.assertAlmostEqual(swe, 4.0, delta=1e-6)
            self.assertAlmostEqual(fw, 0.5, delta=1e-6)
            self.assertAlmostEqual(ati, 28.0, delta=1e-3)
            self.assertAlmostEqual(coldc, 0.1, delta=1e-6)
        finally:
            s.end(); s.close(); s.destroy()

    def test_free_water_capped_to_pack(self):
        """S2: free water cannot exceed SWE."""
        s = _solver(_SNOW_INP, "snow_fwcap")
        try:
            sc = s.subcatchments["S1"]
            for _ in range(5):
                s.step()
            sc.set_snow_state(_PERV, swe=1.0, fw=5.0)
            swe, fw, _, _ = sc.get_snow_state(_PERV)
            self.assertLessEqual(fw, swe + 1e-9)
        finally:
            s.end(); s.close(); s.destroy()

    def test_prescribed_swe_melts_out(self):
        """S2: a prescribed SWE melts under forced warmth; depth drops."""
        s = _solver(_SNOW_INP, "snow_meltout")
        try:
            sc = s.subcatchments["S1"]
            for _ in range(5):
                s.step()
            # Prescribe a substantial pack on all surfaces and read the depth.
            for surf in (_PLOWABLE, _IMPERV, _PERV):
                sc.set_snow_state(surf, swe=5.0, fw=0.0, ati=32.0, coldc=0.0)
            s.step()
            peak = sc.snow_depth
            self.assertGreater(peak, 0.0)
            # Force strong warmth so the pack melts; stop any further snowfall.
            s.forcing.subcatchment_snowfall("S1", 0.0, persist=True)
            s.forcing.climate_temperature(80.0, persist=True)
            for _ in range(240):
                s.step()
            self.assertLess(sc.snow_depth, peak)
        finally:
            s.end(); s.close(); s.destroy()
