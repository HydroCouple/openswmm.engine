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
