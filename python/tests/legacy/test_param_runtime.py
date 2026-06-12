# Description: Phase 4 parameter-editing-surface — legacy parity setters.
#
# Covers the legacy side of docs/RUNTIME_FORCING_PHASE4_HANDOFF.md §2 (gap-plan
# §12). The legacy engine previously had no runtime setters for time-pattern
# factors or land-use street-sweeping parameters (input-only); this adds the
# parity setters that match the refactored contract.
#
# Wave B1 — P6 time patterns (swmm_TIME_PATTERN / swmm_PATTERN_FACTOR), P4
# street sweeping (swmm_LANDUSE / swmm_LANDUSE_SWEEP_*). Both are looked up per
# step, so a mid-run edit takes effect on the next step (sound).
#
# Real legacy solver only; artifacts under tests/legacy/output.
#
# Created on: 2026-06-12

import os
import re

import pytest

from openswmm import solver
from openswmm.legacy.engine import LegacyNodes

_LEGACY_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_LEGACY_DIR)))
_OUT_DIR = os.path.join(_LEGACY_DIR, "output")
_LEGACY_SMALL = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "legacy_small.inp")
_SITE_DRAINAGE = os.path.join(
    _REPO_ROOT, "python", "tests", "data", "solver", "site_drainage_example.inp")

_TP = solver.SWMMObjects.TIME_PATTERN
_LU = solver.SWMMObjects.LANDUSE
_PatProp = solver.SWMMPatternProperties
_LuProp = solver.SWMMLandUseProperties


def _derive_dwf_model():
    """Runnable one-day copy of legacy_small (DWF + patterns, no rain needed).

    Replaces the missing external rain FILE with a single zero inline row and
    drops the orphan external stage FILE timeseries so the run starts.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_LEGACY_SMALL) as f:
        txt = f.read()
    txt = re.sub(r'^NOAA_RIC_2004_2022\s+FILE\s+"[^"]*"\s*$',
                 'NOAA_RIC_2004_2022 10/07/2012 00:00 0.0',
                 txt, count=1, flags=re.MULTILINE)
    txt = re.sub(r'^USGS_James_River_2002_2022\s+FILE\s+"[^"]*"\s*$', '',
                 txt, count=1, flags=re.MULTILINE)
    txt = re.sub(r'^END_DATE\s+\S+', 'END_DATE             10/08/2012',
                 txt, count=1, flags=re.MULTILINE)
    path = os.path.join(_OUT_DIR, "param_dwf.inp")
    with open(path, "w") as f:
        f.write(txt)
    return path


def _open(inp, name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, name)
    s = solver.Solver(inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")
    s.initialize()
    return s


# --------------------------------------------------------------------------- #
# P6 — time-pattern factors (legacy parity)
# --------------------------------------------------------------------------- #
class TestLegacyPatternFactors:
    def _biggest_dwf_node(self, s, nodes):
        idx = max(range(len(nodes)), key=lambda i: nodes[i].lateral_inflow)
        return idx, nodes[idx].lateral_inflow

    def test_pattern_factor_round_trip(self):
        """P6: set/get one pattern factor, plus read-only count/type."""
        s = _open(_derive_dwf_model(), "p6_legacy_rt")
        try:
            npat = s.get_object_count(_TP)
            assert npat > 0
            count = int(s.get_value(_TP, _PatProp.COUNT, 0))
            assert count > 0
            s.set_value(_TP, _PatProp.FACTOR, 0, 2.5, sub_index=0)
            assert s.get_value(_TP, _PatProp.FACTOR, 0, sub_index=0) == pytest.approx(2.5)
        finally:
            s.end(); s.finalize()

    def test_pattern_edit_changes_dwf_next_step(self):
        """P6: scaling all pattern factors mid-run changes DWF inflow."""
        s = _open(_derive_dwf_model(), "p6_legacy_effect")
        try:
            nodes = LegacyNodes(s)
            for _ in range(12):
                s.step()
            idx, before = self._biggest_dwf_node(s, nodes)
            assert before > 1e-9, "expected a DWF-fed node"
            npat = s.get_object_count(_TP)
            for p in range(npat):
                cnt = int(s.get_value(_TP, _PatProp.COUNT, p))
                for k in range(cnt):
                    s.set_value(_TP, _PatProp.FACTOR, p, 10.0, sub_index=k)
            s.step()
            after = nodes[idx].lateral_inflow
            assert after > before * 1.5, (before, after)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P4 — street sweeping (legacy parity)
# --------------------------------------------------------------------------- #
class TestLegacySweep:
    def test_sweep_params_round_trip(self):
        """P4: set/get land-use sweep interval and removal mid-run."""
        s = _open(_SITE_DRAINAGE, "p4_legacy_rt")
        try:
            nlu = s.get_object_count(_LU)
            assert nlu > 0, "fixture must define land uses"
            for _ in range(5):
                s.step()
            s.set_value(_LU, _LuProp.SWEEP_INTERVAL, 0, 5.0)
            s.set_value(_LU, _LuProp.SWEEP_REMOVAL, 0, 0.65)
            assert s.get_value(_LU, _LuProp.SWEEP_INTERVAL, 0) == pytest.approx(5.0)
            assert s.get_value(_LU, _LuProp.SWEEP_REMOVAL, 0) == pytest.approx(0.65)
        finally:
            s.end(); s.finalize()

    def test_sweep_removal_bounds_rejected(self):
        """P4: an out-of-range removal fraction is rejected."""
        s = _open(_SITE_DRAINAGE, "p4_legacy_bounds")
        try:
            with pytest.raises(Exception):
                s.set_value(_LU, _LuProp.SWEEP_REMOVAL, 0, 1.5)
        finally:
            s.end(); s.finalize()
