"""2D mesh evaporation forcing tests — plan row T1.

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md item T1 and the §4.1 cleanup:
  * ``Surface2D.force_evap_uniform`` / ``force_evap`` apply an evaporation
    sink on the 2D mesh.
  * The evaporation loss is accumulated into the 2D mass balance and reported
    via ``get_mass_balance()["evap_out"]`` (the MassBalance2D.evap_out field
    added in the §4.1 consolidation), scaling with the prescribed rate.
  * A RESET (one-shot) prescription applies for a single step.

The bundled ``examples/2d_complete_example.inp`` is a *coupled* 1D-2D model
(its mesh drains to 1D nodes), so a closed volumetric balance is dominated by
the coupling terms; these tests therefore assert the evaporation sink's
behaviour (presence, rate-scaling, one-shot reset) rather than full 2D
continuity closure, which requires an isolated mesh fixture.

No mocks; skips cleanly when the build lacks 2D support.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._2d")

from openswmm.engine import Solver, ForcingPersist

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
_TWOD_INP = os.path.join(_REPO_ROOT, "examples", "2d_complete_example.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")


def _solver(name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, name)
    s = Solver(_TWOD_INP, base + ".rpt", base + ".out")
    s.open()
    s.initialize()
    s.start()
    return s


_MM_HR_TO_MS = 1.0 / (1000.0 * 3600.0)  # mm/hr → m/s


def _run_with_evap(rate_mm_hr, n=30, rain_mm_hr=30.0):
    """Run the mesh under steady rain + forced evap; return final mass balance.

    Continuous rainfall keeps the cells wet so the evaporation sink has water
    to act on (the coupled mesh otherwise drains to the 1D network).
    """
    s = _solver(f"twod_evap_{rate_mm_hr}")
    try:
        surf = s.surface2d
        surf.force_rainfall_uniform(rain_mm_hr * _MM_HR_TO_MS, persist=ForcingPersist.PERSIST)
        if rate_mm_hr > 0.0:
            surf.force_evap_uniform(rate_mm_hr * _MM_HR_TO_MS, persist=ForcingPersist.PERSIST)
        for _ in range(n):
            s.step()
        return surf.get_mass_balance()
    finally:
        s.end(); s.close(); s.destroy()


class TestSurface2dEvap:
    def test_evap_out_key_present(self):
        """§4.1: get_mass_balance exposes the evap_out term, non-negative."""
        mb = _run_with_evap(0.0)
        assert "evap_out" in mb
        assert mb["evap_out"] >= 0.0

    def test_forced_evap_produces_loss(self):
        """T1: forcing evaporation accumulates a positive evap_out."""
        mb = _run_with_evap(8.0)
        assert mb["evap_out"] > 0.0

    def test_evap_out_scales_with_rate(self):
        """T1: doubling the evaporation rate ~doubles the accumulated loss."""
        low = _run_with_evap(5.0)["evap_out"]
        high = _run_with_evap(10.0)["evap_out"]
        assert low > 0.0
        # Same wet area and steps, so the loss scales with the rate (allow a
        # generous tolerance for depth-limited dry-out at the higher rate).
        assert high == pytest.approx(2.0 * low, rel=0.25)

    def test_one_shot_reset_applies_single_step(self):
        """T1: a RESET (persist=False) evap prescription lasts one step."""
        s = _solver("twod_evap_oneshot")
        try:
            surf = s.surface2d
            surf.force_rainfall_uniform(30.0 * _MM_HR_TO_MS, persist=ForcingPersist.PERSIST)
            for _ in range(5):
                s.step()
            before = surf.get_mass_balance()["evap_out"]
            # One-shot evap (default RESET persist): one step of loss.
            surf.force_evap_uniform(10.0 * _MM_HR_TO_MS)
            s.step()
            after_one = surf.get_mass_balance()["evap_out"]
            assert after_one > before
            # No further evap forcing → evap_out stops growing.
            delta1 = after_one - before
            s.step()
            s.step()
            after_more = surf.get_mass_balance()["evap_out"]
            # The post-reset growth is at most the single-step delta's noise,
            # i.e. far less than if the prescription had persisted.
            assert (after_more - after_one) <= delta1 * 0.5 + 1e-9
        finally:
            s.end(); s.close(); s.destroy()
