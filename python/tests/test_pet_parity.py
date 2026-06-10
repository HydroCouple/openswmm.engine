"""Cross-engine parity tests for the subcatchment PET prescription.

Covers docs/SUBCATCHMENT_PET_PRESCRIPTION_PLAN.md section 6, cases 8 and
10: identical models and prescription schedules produce matching
per-subcatchment evaporation in the legacy and refactored engines, and a
groundwater-coupled model responds to the prescribed rate.

All tests run against the real solvers (no mocks). Report/output files
are written to ``tests/output_pet_parity`` so they remain reviewable.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

from openswmm import solver as legacy_solver
from openswmm.engine import Solver as EngineSolver

_THIS_DIR = os.path.dirname(__file__)
_SITE_INP = os.path.join(_THIS_DIR, "data", "solver",
                         "site_drainage_example.inp")
# Groundwater-coupled twins maintained for C-level parity testing.
_REPO_ROOT = os.path.dirname(os.path.dirname(_THIS_DIR))
_GW_LEGACY_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "legacy_small.inp")
_GW_ENGINE_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "refactored_small.inp")

_OUT_DIR = os.path.join(_THIS_DIR, "output_pet_parity")

_PET = 2.4            # in/day (site model is US units)
_SPINUP_STEPS = 30
_COMPARE_STEPS = 20
_PARITY_RTOL = 0.05   # established legacy-parity tolerance for hydrology


def _out_base(name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    return os.path.join(_OUT_DIR, name)


def test_per_step_evap_parity():
    """Case 10: per-step S1 evap matches between engines under prescription."""
    base_l = _out_base("parity_legacy")
    base_e = _out_base("parity_engine")

    # --- legacy run ---
    ls = legacy_solver.Solver(
        inp_file=_SITE_INP, rpt_file=base_l + ".rpt", out_file=base_l + ".out")
    ls.initialize()
    from openswmm.legacy.engine import LegacySubcatchments
    lsubs = LegacySubcatchments(ls)
    for _ in range(_SPINUP_STEPS):
        ls.step()
    lsubs["S1"].set_api_pet(_PET)
    legacy_series = []
    for _ in range(_COMPARE_STEPS):
        ls.step()
        legacy_series.append(lsubs["S1"].evaporation)
    ls.finalize()

    # --- refactored run ---
    es = EngineSolver(_SITE_INP, base_e + ".rpt", base_e + ".out")
    es.open()
    es.initialize()
    es.start()
    for _ in range(_SPINUP_STEPS):
        es.step()
    engine_series = []
    for _ in range(_COMPARE_STEPS):
        # Refactored prescription is per-step here (one-shot) to mirror the
        # sticky legacy prescription exactly.
        es.forcing.subcatchment_evap("S1", _PET, persist=False)
        es.step()
        engine_series.append(es.subcatchments["S1"].evap)
    es.end()
    es.close()
    es.destroy()

    assert len(legacy_series) == len(engine_series)
    for i, (lv, ev) in enumerate(zip(legacy_series, engine_series)):
        assert ev == pytest.approx(lv, rel=_PARITY_RTOL, abs=1e-9), (
            f"step {i}: legacy={lv} engine={ev}")


def test_groundwater_model_responds_to_prescription():
    """Case 8: prescribed PET changes evaporation on a GW-coupled model.

    The GW twins use MONTHLY climate evaporation, so the baseline run has
    non-zero climate-driven evap; prescribing 0.0 must suppress it and
    prescribing a large rate must exceed it, in both engines.
    """
    if not (os.path.isfile(_GW_LEGACY_INP) and os.path.isfile(_GW_ENGINE_INP)):
        pytest.skip("groundwater parity models not present")

    n_steps = 500  # 5 s routing step → ~42 minutes of October simulation

    def legacy_total_evap(prescribed):
        base = _out_base(f"gw_legacy_{prescribed}")
        s = legacy_solver.Solver(
            inp_file=_GW_LEGACY_INP,
            rpt_file=base + ".rpt", out_file=base + ".out")
        s.initialize()
        from openswmm.legacy.engine import LegacySubcatchments
        subs = LegacySubcatchments(s)
        if prescribed is not None:
            for sub in subs:
                sub.set_api_pet(prescribed)
        total = 0.0
        for _ in range(n_steps):
            s.step()
            total += sum(sub.evaporation for sub in subs)
        s.finalize()
        return total

    def engine_total_evap(prescribed):
        base = _out_base(f"gw_engine_{prescribed}")
        s = EngineSolver(_GW_ENGINE_INP, base + ".rpt", base + ".out")
        s.open()
        s.initialize()
        s.start()
        n_sub = len(s.subcatchments)
        total = 0.0
        for _ in range(n_steps):
            if prescribed is not None:
                for i in range(n_sub):
                    s.forcing.subcatchment_evap(i, prescribed, persist=False)
            s.step()
            total += sum(sc.evap for sc in s.subcatchments)
        s.end()
        s.close()
        s.destroy()
        return total

    for total_fn in (legacy_total_evap, engine_total_evap):
        baseline = total_fn(None)     # climate-driven (MONTHLY, October)
        suppressed = total_fn(0.0)    # prescribe zero PET
        boosted = total_fn(10.0)      # prescribe large PET (in/day)
        assert suppressed <= baseline or baseline == 0.0
        assert boosted >= baseline
        # Prescribing zero must fully suppress potential evaporation.
        assert suppressed == pytest.approx(0.0, abs=1e-9)
