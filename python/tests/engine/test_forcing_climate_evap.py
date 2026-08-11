"""Climate evaporation forcing tests (refactored engine) — rows M4, M5.

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md:
  * M4 — system-wide evaporation override (``Forcing.climate_evap``): it
    drives subcatchment evaporation when no per-subcatchment PET is set, and
    a per-subcatchment ``subcatchment_evap`` prescription wins over it.
  * M5 — runtime DRY_ONLY toggle (``Forcing.climate_dry_only``): turning it on
    mid-storm suppresses surface evaporation while it rains; turning it off
    resumes it.

All tests run against the real handle-based ``openswmm.engine.Solver``
(no mocks). Report/output files land in ``tests/engine/output``.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver, ForcingMode

_DATA_DIR = os.path.join(
    os.path.dirname(os.path.dirname(__file__)), "data", "solver")
_INP = os.path.join(_DATA_DIR, "site_drainage_example.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

# Deterministic runoff clock so a prescription is observable on the next step
# (see test_forcing_pet for the rationale).
_DETERMINISTIC = (
    ("WET_STEP             00:01:00", "WET_STEP             00:00:15"),
    ("VARIABLE_STEP        0.75", "VARIABLE_STEP        0.0"),
)


def _model(name, replacements=()):
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_INP) as f:
        text = f.read()
    for old, new in (*_DETERMINISTIC, *replacements):
        assert old in text, f"expected {old!r} in template model"
        text = text.replace(old, new)
    path = os.path.join(_OUT_DIR, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def _solver(name, replacements=()):
    base = os.path.join(_OUT_DIR, name)
    s = Solver(_model(name + ".inp", replacements), base + ".rpt", base + ".out")
    s.open()
    s.initialize()
    s.start()
    return s


class TestGlobalEvapOverride(unittest.TestCase):
    def test_drives_subcatchment_evap(self):
        """M4: a global prescription evaporates from every subcatchment."""
        s = _solver("cevap_global")
        try:
            for _ in range(20):
                s.step()
            s.forcing.climate_evap(1.2, persist=True)
            s.step()
            self.assertAlmostEqual(
                s.forcing.climate_evap_rate(), 1.2, delta=1.2e-6)
            # Both subcatchments evaporate (PET-limited, so up to the rate).
            self.assertGreater(s.subcatchments["S1"].evap, 0.0)
            self.assertLessEqual(s.subcatchments["S1"].evap, 1.2 * 1.01)
            self.assertGreater(s.subcatchments["S2"].evap, 0.0)
            self.assertLessEqual(s.subcatchments["S2"].evap, 1.2 * 1.01)
        finally:
            s.end(); s.close(); s.destroy()

    def test_per_subcatch_overrides_global(self):
        """M4: per-subcatchment PET wins over the global prescription."""
        s = _solver("cevap_precedence")
        try:
            for _ in range(20):
                s.step()
            s.forcing.climate_evap(1.0, persist=True)
            s.forcing.subcatchment_evap("S1", 3.0, persist=True)
            s.step()
            # S1 follows its own (higher) PET; S2 follows the global rate.
            self.assertGreater(s.subcatchments["S1"].evap, 1.5)
            self.assertLessEqual(s.subcatchments["S2"].evap, 1.0 * 1.01)
        finally:
            s.end(); s.close(); s.destroy()

    def test_continuity_with_global_evap(self):
        """M4: a persistent global prescription keeps continuity sound."""
        s = _solver("cevap_continuity")
        s.forcing.climate_evap(1.0, persist=True)
        while s.step():
            s.forcing.climate_evap(1.0, persist=True)
        s.end()
        try:
            self.assertLess(abs(s.mass_balance.runoff_continuity_error), 0.5)
        finally:
            s.close(); s.destroy()


class TestDryOnlyToggle(unittest.TestCase):
    def test_toggle_suppresses_and_resumes(self):
        """M5: DRY_ONLY on suppresses evap during rain; off resumes it."""
        s = _solver("cevap_dryonly")
        try:
            s.forcing.climate_evap(2.0, persist=True)
            # Step into the active storm.
            for _ in range(4):
                s.step()
            self.assertGreater(
                s.subcatchments["S1"].evap, 0.0)  # evaporating during rain

            # Turn DRY_ONLY on: surface evap stops while it rains.
            s.forcing.climate_dry_only(True)
            self.assertIs(s.forcing.get_climate_dry_only(), True)
            s.forcing.climate_evap(2.0, persist=True)
            s.step()
            self.assertAlmostEqual(
                s.subcatchments["S1"].evap, 0.0, delta=1e-12)

            # Turn it back off: evap resumes.
            s.forcing.climate_dry_only(False)
            self.assertIs(s.forcing.get_climate_dry_only(), False)
            s.forcing.climate_evap(2.0, persist=True)
            s.step()
            self.assertGreater(s.subcatchments["S1"].evap, 0.0)
        finally:
            s.end(); s.close(); s.destroy()
