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
_PARITY_RTOL = 0.05   # established legacy-parity tolerance for hydrology

# The two engines advance their public step() on different clocks (legacy on
# the report step, refactored on the routing step), so an instantaneous
# per-step evaporation comparison samples different moments of the storm and
# is not well-posed — at a tapering moment the zero-depression impervious
# subarea has drained (no water to evaporate) while at an intense moment it is
# ponded. The integral over the run is sampling-independent and is the
# meaningful parity quantity; both engines conserve mass and apply the
# prescription to available water identically.
_DETERMINISTIC = (
    ("WET_STEP             00:01:00", "WET_STEP             00:00:15"),
    ("VARIABLE_STEP        0.75", "VARIABLE_STEP        0.0"),
)


def _out_base(name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    return os.path.join(_OUT_DIR, name)


def _deterministic_site_model():
    with open(_SITE_INP) as f:
        text = f.read()
    for old, new in _DETERMINISTIC:
        assert old in text, f"expected {old!r} in site model"
        text = text.replace(old, new)
    path = _out_base("parity_site.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


def test_total_evap_parity():
    """Case 10: total prescribed-PET evaporation matches between engines.

    S1 is given a constant PET for the entire run in both engines; the
    integrated runoff-evaporation total must match (the other subcatchments
    have climate evap 0.0, so the system total is S1's contribution).
    """
    inp = _deterministic_site_model()
    base_l = _out_base("parity_legacy")
    base_e = _out_base("parity_engine")

    # --- legacy run (sticky prescription) ---
    from openswmm.legacy.engine import LegacySubcatchments, LegacySystem
    ls = legacy_solver.Solver(
        inp_file=inp, rpt_file=base_l + ".rpt", out_file=base_l + ".out")
    ls.initialize()
    lsubs = LegacySubcatchments(ls)
    lsubs["S1"].set_api_pet(_PET)
    while ls.solver_state != legacy_solver.SolverState.FINISHED:
        ls.step()
        lsubs["S1"].set_api_pet(_PET)
    ls.end()
    legacy_total = LegacySystem(ls).runoff_totals["evap"]
    ls.finalize()

    # --- refactored run (re-prescribe each step) ---
    from openswmm.engine import RunoffTotal
    es = EngineSolver(inp, base_e + ".rpt", base_e + ".out")
    es.open()
    es.initialize()
    es.start()
    while es.step():
        es.forcing.subcatchment_evap("S1", _PET, persist=True)
    es.end()
    engine_total = es.mass_balance.runoff_total(RunoffTotal.EVAP)
    es.close()
    es.destroy()

    assert legacy_total > 0.0
    assert engine_total == pytest.approx(legacy_total, rel=_PARITY_RTOL)


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
