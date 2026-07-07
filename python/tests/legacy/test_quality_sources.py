# Description: Legacy pollutant source-concentration setters (rows Q1-Q3, Q6;
# plus Q5 DWF as the natural fourth source).
#
# Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md §7.1/§7.3 legacy side and
# docs/RUNTIME_FORCING_PHASE4_HANDOFF.md §3. The legacy setPollutValue
# branch (swmm5.c) writes Pollut[].pptConcen/gwConcen/rdiiConcen/dwfConcen,
# settable both before and during the run; the ponded setter writes
# Subcatch[].pondedQual. Each source feeds an existing inflow term already
# counted in the quality mass balance, so no new accounting is introduced.
#
# Fixtures (none of the bundled models had pollutants AND the right inflows):
#   * Q1 rain / Q6 ponded -> site_drainage_example.inp: a self-contained
#     single-day storm that already carries a TSS pollutant with
#     buildup/washoff, so wet-deposition and ponded injection are observable.
#   * Q2 GW / Q3 RDII / Q5 DWF -> a derived legacy_small.inp: that model wires
#     real [GROUNDWATER]/[RDII]/[DWF] inflows to nodes but ships with its rain
#     as a missing external FILE, IGNORE_QUALITY YES, and a year-long horizon.
#     The derived copy injects a self-contained inline storm, enables quality
#     routing, drops the orphan stage FILE timeseries (an unused FREE outfall),
#     trims the run to two days, and appends a single TSS pollutant whose
#     source columns are set one at a time so each node signal is attributable
#     to exactly one source.
#
# Real legacy solver only (no mocks); rpt/out artifacts land in
# tests/legacy/output so they stay reviewable.
#
# Created on: 2026-06-12

import os
import re

import pytest

from openswmm import solver
from openswmm.legacy.engine import LegacyNodes, LegacySubcatchments, LegacySystem

_LEGACY_DIR = os.path.dirname(os.path.abspath(__file__))
# .../<repo>/python/tests/legacy -> .../<repo>
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_LEGACY_DIR)))
_OUT_DIR = os.path.join(_LEGACY_DIR, "output")
_LEGACY_SMALL = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "legacy_small.inp")
_SITE_DRAINAGE = os.path.join(
    _REPO_ROOT, "python", "tests", "data", "solver", "site_drainage_example.inp")

_GAGE = "NOAA_RIC_2004_2022"
_PObj = solver.SWMMObjects.POLLUTANT
_PProp = solver.SWMMPollutantProperties


# --------------------------------------------------------------------------- #
# Fixture builders
# --------------------------------------------------------------------------- #
def _inline_storm():
    """5-min INTENSITY rows: a heavy 4-hour storm on 10/07/2012."""
    rows, h, m = [], 2, 0
    while (h, m) < (6, 0):
        rows.append(f"{_GAGE} 10/07/2012 {h:02d}:{m:02d} 1.0")
        m += 5
        if m == 60:
            m, h = 0, h + 1
    return "\n".join(rows)


def _derive_legacy_small(crain, cgw, crdii, cdwf, tag):
    """Self-contained 2-day storm copy of legacy_small with one TSS pollutant."""
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_LEGACY_SMALL) as f:
        txt = f.read()
    txt = re.sub(rf'^{_GAGE}\s+FILE\s+"[^"]*"\s*$', _inline_storm(),
                 txt, count=1, flags=re.MULTILINE)
    txt = re.sub(r'^USGS_James_River_2002_2022\s+FILE\s+"[^"]*"\s*$', '',
                 txt, count=1, flags=re.MULTILINE)
    txt = re.sub(r'^END_DATE\s+\S+', 'END_DATE             10/09/2012',
                 txt, count=1, flags=re.MULTILINE)
    txt = re.sub(r'^IGNORE_QUALITY\s+\S+', 'IGNORE_QUALITY       NO',
                 txt, count=1, flags=re.MULTILINE)
    txt += (
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        f"TSS MG/L {crain} {cgw} {crdii} 0.0 NO * 0.0 {cdwf} 0.0\n"
    )
    path = os.path.join(_OUT_DIR, f"quality_src_{tag}.inp")
    with open(path, "w") as f:
        f.write(txt)
    return path


def _open(inp, name):
    os.makedirs(_OUT_DIR, exist_ok=True)
    base = os.path.join(_OUT_DIR, name)
    s = solver.Solver(inp_file=inp, rpt_file=base + ".rpt", out_file=base + ".out")
    s.initialize()
    return s


def _max_node_quality(s, max_steps):
    """Step until some node carries the pollutant; return (peak, name)."""
    nodes = LegacyNodes(s)
    peak, who = 0.0, ""
    steps = 0
    while s.solver_state != solver.SolverState.FINISHED and steps < max_steps:
        s.step()
        steps += 1
        for i in range(len(nodes)):
            q = nodes[i].get_pollutant_concentration(0)
            if q > peak:
                peak, who = q, nodes[i].name
        if peak > 1.0:           # signal found; no need to run further
            break
    return peak, who


# --------------------------------------------------------------------------- #
# Q1 — rain (wet-deposition) concentration
# --------------------------------------------------------------------------- #
class TestRainConcentration:
    def _peak_runoff_tss(self, rain_conc, name):
        s = _open(_SITE_DRAINAGE, name)
        s.set_value(_PObj, _PProp.RAIN_CONCENTRATION, 0, rain_conc)
        subs = LegacySubcatchments(s)
        peak = 0.0
        try:
            while s.solver_state != solver.SolverState.FINISHED:
                s.step()
                for i in range(len(subs)):
                    c = subs[i].get_pollutant_runoff_concentration(0)
                    if c > peak:
                        peak = c
        finally:
            s.end(); s.finalize()
        return peak

    def test_rain_concentration_drives_washoff(self):
        """Q1: wet-deposition concentration appears in subcatchment runoff."""
        assert self._peak_runoff_tss(0.0, "q1_rain_off") == pytest.approx(0.0, abs=1e-6)
        assert self._peak_runoff_tss(100.0, "q1_rain_on") > 50.0

    def test_rain_concentration_round_trip(self):
        """Q1: the rain-concentration getter round-trips a mid-run set."""
        s = _open(_SITE_DRAINAGE, "q1_round_trip")
        try:
            for _ in range(5):
                s.step()
            s.set_value(_PObj, _PProp.RAIN_CONCENTRATION, 0, 73.0)
            assert s.get_value(_PObj, _PProp.RAIN_CONCENTRATION, 0) == pytest.approx(73.0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# Q2 — groundwater inflow concentration
# --------------------------------------------------------------------------- #
class TestGwConcentration:
    def test_gw_concentration_reaches_node(self):
        """Q2: with Cgw>0 only, a GW-fed node carries the pollutant."""
        inp = _derive_legacy_small(0.0, 50.0, 0.0, 0.0, "gw")
        s = _open(inp, "q2_gw")
        try:
            peak, who = _max_node_quality(s, max_steps=600)
            assert peak > 1.0, f"expected GW-sourced node TSS>1 (got {peak} at {who})"
        finally:
            s.end(); s.finalize()

    def test_gw_concentration_round_trip(self):
        """Q2: GW-concentration getter round-trips a mid-run set."""
        inp = _derive_legacy_small(0.0, 50.0, 0.0, 0.0, "gw")
        s = _open(inp, "q2_gw_rt")
        try:
            for _ in range(5):
                s.step()
            assert s.get_value(_PObj, _PProp.GW_CONCENTRATION, 0) == pytest.approx(50.0)
            s.set_value(_PObj, _PProp.GW_CONCENTRATION, 0, 120.0)
            assert s.get_value(_PObj, _PProp.GW_CONCENTRATION, 0) == pytest.approx(120.0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# Q3 — RDII inflow concentration
# --------------------------------------------------------------------------- #
class TestRdiiConcentration:
    def test_rdii_concentration_reaches_node(self):
        """Q3: with Crdii>0 only, an RDII-fed node carries the pollutant."""
        inp = _derive_legacy_small(0.0, 0.0, 50.0, 0.0, "rdii")
        s = _open(inp, "q3_rdii")
        try:
            peak, who = _max_node_quality(s, max_steps=600)
            assert peak > 1.0, f"expected RDII-sourced node TSS>1 (got {peak} at {who})"
        finally:
            s.end(); s.finalize()

    def test_rdii_concentration_round_trip(self):
        """Q3: RDII-concentration getter round-trips a mid-run set."""
        inp = _derive_legacy_small(0.0, 0.0, 50.0, 0.0, "rdii")
        s = _open(inp, "q3_rdii_rt")
        try:
            for _ in range(5):
                s.step()
            assert s.get_value(_PObj, _PProp.RDII_CONCENTRATION, 0) == pytest.approx(50.0)
            s.set_value(_PObj, _PProp.RDII_CONCENTRATION, 0, 33.0)
            assert s.get_value(_PObj, _PProp.RDII_CONCENTRATION, 0) == pytest.approx(33.0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# Q5 (legacy) — dry-weather-flow concentration
# --------------------------------------------------------------------------- #
class TestDwfConcentration:
    def test_dwf_concentration_reaches_node(self):
        """Q5: with Cdwf>0 only, a pure-DWF node concentration equals Cdwf."""
        inp = _derive_legacy_small(0.0, 0.0, 0.0, 50.0, "dwf")
        s = _open(inp, "q5_dwf")
        try:
            peak, who = _max_node_quality(s, max_steps=600)
            # A node fed solely by DWF mixes to the prescribed concentration.
            assert peak == pytest.approx(50.0, rel=0.2), f"got {peak} at {who}"
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# Q6 — ponded surface-quality injection
# --------------------------------------------------------------------------- #
class TestPondedInjection:
    def _run_to_ponding(self, s):
        """Step until a subcatchment has active runoff (surface depth > 0)."""
        subs = LegacySubcatchments(s)
        for _ in range(60):
            if s.solver_state == solver.SolverState.FINISHED:
                break
            s.step()
            for i in range(len(subs)):
                if subs[i].runoff > 1e-7:
                    return subs, i
        return subs, -1

    def test_ponded_concentration_round_trip(self):
        """Q6: the ponded-concentration setter/getter invert exactly mid-storm."""
        s = _open(_SITE_DRAINAGE, "q6_round_trip")
        try:
            subs, idx = self._run_to_ponding(s)
            assert idx >= 0, "no subcatchment developed ponded depth"
            subs[idx].set_ponded_concentration(0, 500.0)
            back = s.get_value(
                solver.SWMMObjects.SUBCATCHMENT,
                solver.SWMMSubcatchmentProperties.POLLUTANT_PONDED_CONCENTRATION,
                idx, pollutant_index=0)
            assert back == pytest.approx(500.0, rel=1e-6)
        finally:
            s.end(); s.finalize()

    def test_ponded_injection_washes_off(self):
        """Q6: injected ponded mass adds to the subcatchment washoff load."""
        def total_load(inject):
            s = _open(_SITE_DRAINAGE, f"q6_load_{int(inject)}")
            subs, idx = self._run_to_ponding(s)
            if idx >= 0 and inject:
                subs[idx].set_ponded_concentration(0, 500.0)
            load = 0.0
            try:
                while s.solver_state != solver.SolverState.FINISHED:
                    s.step()
                load = subs[idx].get_pollutant_total_load(0) if idx >= 0 else 0.0
            finally:
                s.end(); s.finalize()
            return load, idx

        base_load, idx = total_load(False)
        inj_load, _ = total_load(True)
        assert idx >= 0
        # The injected ponded mass washes off, so the cumulative load strictly
        # exceeds the no-injection baseline.
        assert inj_load > base_load
