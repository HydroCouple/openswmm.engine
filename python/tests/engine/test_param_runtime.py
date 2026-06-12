"""Phase 4 parameter-editing-surface audits — refactored engine.

Pins down the mid-run mutation semantics of the refactored parameter setters
(docs/RUNTIME_FORCING_PHASE4_HANDOFF.md §2, gap-plan §12). For each setter
group: set the parameter mid-run, assert it takes effect on the next step and
that no state corruption results.

Wave B1 — P6 time patterns, P4 street sweeping.

P6 contract: DWF/external-inflow pattern factors are cached in the inflow
solver for per-step performance; ``swmm_pattern_set_factors`` now refreshes
that cache so a mid-run edit takes effect on the next step. (Groundwater-evap
patterns already read the live context.)

P4 contract: ``Landuse.sweep_interval`` / ``sweep_removal`` write the live
context vectors read each step when sweeping is evaluated — trivially sound.

Real handle-based solver; artifacts under tests/engine/output.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

from openswmm.engine import Solver

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
_DWF_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "refactored_small.inp")
_LANDUSE_INP = os.path.join(
    _REPO_ROOT, "python", "tests", "data", "solver", "site_drainage_example.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")


def _open(inp, name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, name)
    s = Solver(inp, base + ".rpt", base + ".out")
    s.open(); s.initialize(); s.start()
    return s


# --------------------------------------------------------------------------- #
# P6 — time-pattern factors
# --------------------------------------------------------------------------- #
class TestPatternFactorsRuntime:
    def _biggest_dwf_node(self, s):
        idx = max(range(len(s.nodes)), key=lambda i: s.nodes[i].lateral_inflow)
        return s.nodes[idx]

    def test_pattern_edit_changes_dwf_next_step(self):
        """P6: scaling pattern factors mid-run changes DWF inflow next step."""
        s = _open(_DWF_INP, "p6_pattern")
        try:
            for _ in range(12):
                s.step()
            node = self._biggest_dwf_node(s)
            before = node.lateral_inflow
            assert before > 1e-9, "expected a DWF-fed node"
            # Scale every pattern's factors up 10x.
            for p in s.patterns:
                p.set_factors([x * 10.0 for x in p.factors])
            s.step()
            after = node.lateral_inflow
            assert after > before * 1.5, (
                f"DWF inflow did not respond to pattern edit "
                f"(before={before}, after={after})")
        finally:
            s.end(); s.close(); s.destroy()

    def test_pattern_factors_round_trip(self):
        """P6: the pattern-factor getter reflects a mid-run set."""
        s = _open(_DWF_INP, "p6_round_trip")
        try:
            for _ in range(5):
                s.step()
            p = next(iter(s.patterns))
            n = len(p.factors)
            new = [0.5] * n
            p.set_factors(new)
            assert p.factors == pytest.approx(new)
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P4 — street sweeping
# --------------------------------------------------------------------------- #
class TestSweepRuntime:
    def test_sweep_params_round_trip_no_corruption(self):
        """P4: sweep interval/removal set mid-run round-trip; run stays sane."""
        s = _open(_LANDUSE_INP, "p4_sweep")
        try:
            lus = list(s.quality.landuses)
            assert lus, "fixture must define land uses"
            lu = lus[0]
            for _ in range(10):
                s.step()
            lu.sweep_interval = 5.0
            lu.sweep_removal = 0.65
            assert lu.sweep_interval == pytest.approx(5.0)
            assert lu.sweep_removal == pytest.approx(0.65)
            # Continue: no NaN/Inf, run completes.
            import math
            for _ in range(20):
                if not s.step():
                    break
                for i in range(len(s.nodes)):
                    assert math.isfinite(s.nodes[i].depth)
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P2 — buildup / washoff function coefficients
# --------------------------------------------------------------------------- #
_EMC = 3  # WashoffFunc.EMC — concentration = coeff regardless of buildup


class TestBuildupWashoffRuntime:
    def _first_landuse_pollutant(self, s):
        lus = list(s.quality.landuses)
        assert lus, "fixture must define land uses"
        return lus[0].id, s.pollutants[0].id

    def test_washoff_round_trip(self):
        """P2: set_washoff is reflected by get_washoff mid-run."""
        s = _open(_LANDUSE_INP, "p2_washoff_rt")
        try:
            lu, pol = self._first_landuse_pollutant(s)
            for _ in range(5):
                s.step()
            s.quality.set_washoff(lu, pol, func=_EMC, coeff=123.0, expon=0.0)
            info = s.quality.get_washoff(lu, pol)
            assert info["coeff"] == pytest.approx(123.0)
            assert int(info["func"]) == _EMC
        finally:
            s.end(); s.close(); s.destroy()

    def test_buildup_round_trip(self):
        """P2: set_buildup is reflected by get_buildup mid-run."""
        s = _open(_LANDUSE_INP, "p2_buildup_rt")
        try:
            lu, pol = self._first_landuse_pollutant(s)
            for _ in range(5):
                s.step()
            info0 = s.quality.get_buildup(lu, pol)
            s.quality.set_buildup(lu, pol, func=int(info0["func"]),
                                  c1=88.0, c2=info0["c2"], c3=info0["c3"],
                                  normalizer=info0["normalizer"])
            assert s.quality.get_buildup(lu, pol)["c1"] == pytest.approx(88.0)
        finally:
            s.end(); s.close(); s.destroy()

    def test_washoff_edit_changes_load_next_step(self):
        """P2: an EMC washoff edit takes effect on the next step (cache refresh).

        The per-step path reads a cached LanduseSolver param; swmm_washoff_set
        now refreshes it. With a high EMC coefficient, washoff concentration
        jumps, so downstream node quality rises above the pre-edit baseline.
        """
        s = _open(_LANDUSE_INP, "p2_washoff_effect")
        try:
            lu, pol = self._first_landuse_pollutant(s)
            # Step into the storm so runoff (and washoff) is active.
            for _ in range(15):
                s.step()
            before = max(s.nodes[i].quality(pol) for i in range(len(s.nodes)))
            s.quality.set_washoff(lu, pol, func=_EMC, coeff=5000.0, expon=0.0)
            for _ in range(5):
                s.step()
            after = max(s.nodes[i].quality(pol) for i in range(len(s.nodes)))
            assert after > before + 1.0, (before, after)
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P1 — infiltration parameters
#
# Audit outcome: the refactored infiltration setters are guarded to the
# editable (pre-start) states by CHECK_GEOMETRY — they return SWMM_ERR_LIFECYCLE
# while running. The per-subcatchment infiltration state (Horton decay clock,
# Green-Ampt Fu/Lu) is set up once at start() from these parameters; mid-run
# mutation is therefore NOT supported. This pins the documented contract:
# infiltration parameters are a pre-start edit. (Contrast P6/P2, which the
# engine does allow mid-run and were made cache-coherent.)
# --------------------------------------------------------------------------- #
from openswmm.engine import LifecycleError


class TestInfiltrationParams:
    def test_horton_pre_start_round_trip(self):
        """P1: Horton parameters set before start() take effect and round-trip."""
        os.makedirs(_OUT_DIR, exist_ok=True)
        base = os.path.join(_OUT_DIR, "p1_horton_prestart")
        s = Solver(_LANDUSE_INP, base + ".rpt", base + ".out")
        s.open()
        try:
            sub = s.subcatchments[0]
            sub.infiltration.set_horton(4.5, 0.6, 3.0, 6.0)
            assert sub.infiltration.horton == pytest.approx((4.5, 0.6, 3.0, 6.0))
            # The pre-start edit survives into the run.
            s.initialize(); s.start()
            for _ in range(3):
                s.step()
            assert sub.infiltration.horton == pytest.approx((4.5, 0.6, 3.0, 6.0))
        finally:
            s.end(); s.close(); s.destroy()

    def test_infil_setter_guarded_while_running(self):
        """P1: infiltration setters are rejected mid-run (pre-start-only)."""
        s = _open(_LANDUSE_INP, "p1_guard")
        try:
            for _ in range(5):
                s.step()
            with pytest.raises(LifecycleError):
                s.subcatchments[0].infiltration.set_horton(3.0, 0.5, 4.0, 7.0)
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P5 — pollutant kinetics
#
# Audit: kdecay / co-pollutant / snow-only are read live each step (sound
# mid-run, no cache). init_conc only seeds state at start() and has no per-step
# consumer, so it is now guarded to pre-start (raises LifecycleError mid-run).
# --------------------------------------------------------------------------- #
class TestKineticsRuntime:
    def test_kdecay_round_trip(self):
        """P5: decay constant set mid-run round-trips (read live each step)."""
        s = _open(_LANDUSE_INP, "p5_kdecay_rt")
        try:
            for _ in range(5):
                s.step()
            p = s.pollutants[0]
            p.kdecay = 0.35
            assert p.kdecay == pytest.approx(0.35)
        finally:
            s.end(); s.close(); s.destroy()

    def test_snow_only_round_trip(self):
        """P5: snow-only flag set mid-run round-trips."""
        s = _open(_LANDUSE_INP, "p5_snow_rt")
        try:
            for _ in range(5):
                s.step()
            p = s.pollutants[0]
            p.snow_only = True
            assert bool(p.snow_only) is True
        finally:
            s.end(); s.close(); s.destroy()

    def test_init_conc_guarded_while_running(self):
        """P5: init_conc is rejected mid-run (pre-start-only)."""
        s = _open(_LANDUSE_INP, "p5_initconc_guard")
        try:
            for _ in range(5):
                s.step()
            with pytest.raises(LifecycleError):
                s.pollutants[0].init_conc = 5.0
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P7/P8 — external-inflow / DWF baselines & scale
#
# Audit: the inflow solver caches ext/DWF definitions at start() (same class as
# P6). The new direct setters (swmm_ext_inflow_set_scale/_baseline,
# swmm_dwf_set_baseline) and the add/remove paths now refresh that cache, so a
# mid-run edit takes effect on the next step.
# --------------------------------------------------------------------------- #
_DWF_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "refactored_small.inp")


class TestInflowBaselineRuntime:
    def test_dwf_baseline_edit_changes_inflow(self):
        """P7/P8: raising a DWF baseline mid-run increases the node's inflow."""
        os.makedirs(_OUT_DIR, exist_ok=True)
        base = os.path.join(_OUT_DIR, "p78_dwf")
        s = Solver(_DWF_INP, base + ".rpt", base + ".out")
        s.open(); s.initialize(); s.start()
        try:
            assert s.inflows.dwf_count > 0
            node_idx, constituent, avg = s.inflows.get_dwf(0)[:3]
            for _ in range(10):
                s.step()
            before = s.nodes[node_idx].lateral_inflow
            s.inflows.set_dwf_baseline(0, avg * 20.0 + 1.0)
            s.step()
            after = s.nodes[node_idx].lateral_inflow
            assert after > before + 1e-9, (before, after)
        finally:
            s.end(); s.close(); s.destroy()

    def test_ext_inflow_baseline_round_trip(self):
        """P7: a constant ext-inflow baseline set mid-run round-trips."""
        os.makedirs(_OUT_DIR, exist_ok=True)
        base = os.path.join(_OUT_DIR, "p78_ext")
        s = Solver(_DWF_INP, base + ".rpt", base + ".out")
        s.open()
        try:
            s.inflows.add_external(0, "FLOW", baseline=5.0)
            idx = s.inflows.external_count - 1
            s.initialize(); s.start()
            for _ in range(5):
                s.step()
            s.inflows.set_external_baseline(idx, 42.0)
            s.inflows.set_external_scale(idx, 3.0)
            row = s.inflows.get_external(idx)
            assert row[6] == pytest.approx(42.0)   # baseline
            assert row[5] == pytest.approx(3.0)     # s_factor
        finally:
            s.end(); s.close(); s.destroy()


# --------------------------------------------------------------------------- #
# P3 — treatment expressions
#
# Audit: the step loop evaluates a compiled-expression cache built at start()
# (ctx.treatment.compiled / has_treatment); swmm_treatment_set/_clear now
# recompile the edited (node, pollutant) cell, so a mid-run edit applies on
# the next step. A failed parse is rejected (BadParamError) and the previous
# expression is restored. The start-up cyclic co-treatment check (Gap #85) is
# not re-run for runtime edits.
# --------------------------------------------------------------------------- #
from openswmm.engine import BadParamError


class TestTreatmentRuntime:
    def test_treatment_set_mid_run_reduces_quality(self):
        """P3: a mid-run "R = 0.95" treatment cuts node quality; clear recovers."""
        s = _open(_LANDUSE_INP, "p3_treat_effect")
        try:
            lus = list(s.quality.landuses)
            lu, pol = lus[0].id, s.pollutants[0].id
            for _ in range(15):
                s.step()
            # Drive a strong washoff load so node quality is high.
            s.quality.set_washoff(lu, pol, func=_EMC, coeff=5000.0, expon=0.0)
            for _ in range(5):
                s.step()
            ni = max(range(len(s.nodes)), key=lambda i: s.nodes[i].quality(pol))
            before = s.nodes[ni].quality(pol)
            assert before > 10.0, "expected a washoff-loaded node"
            s.quality.set_treatment(ni, pol, "R = 0.95")
            for _ in range(3):
                s.step()
            treated = s.nodes[ni].quality(pol)
            assert treated < before * 0.5, (before, treated)
            # Clearing the expression lets quality recover.
            s.quality.clear_treatment(ni, pol)
            for _ in range(3):
                s.step()
            recovered = s.nodes[ni].quality(pol)
            assert recovered > treated, (treated, recovered)
        finally:
            s.end(); s.close(); s.destroy()

    def test_treatment_round_trip(self):
        """P3: get_treatment reflects a mid-run set and an empty string after clear."""
        s = _open(_LANDUSE_INP, "p3_treat_rt")
        try:
            pol = s.pollutants[0].id
            for _ in range(5):
                s.step()
            s.quality.set_treatment(0, pol, "R = 0.5")
            assert s.quality.get_treatment(0, pol).strip() == "R = 0.5"
            s.quality.clear_treatment(0, pol)
            assert s.quality.get_treatment(0, pol).strip() == ""
        finally:
            s.end(); s.close(); s.destroy()

    def test_bad_expression_rejected_keeps_previous(self):
        """P3: an unparseable expression raises and the previous one survives."""
        s = _open(_LANDUSE_INP, "p3_treat_bad")
        try:
            pol = s.pollutants[0].id
            for _ in range(5):
                s.step()
            s.quality.set_treatment(0, pol, "R = 0.5")
            with pytest.raises(BadParamError):
                s.quality.set_treatment(0, pol, "not a treatment expr")
            assert s.quality.get_treatment(0, pol).strip() == "R = 0.5"
        finally:
            s.end(); s.close(); s.destroy()
