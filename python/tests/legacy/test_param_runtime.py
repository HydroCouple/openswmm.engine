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

import unittest

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


def _derive_dwf_model(quality=False):
    """Runnable one-day copy of legacy_small (DWF + patterns, no rain needed).

    Replaces the missing external rain FILE with a single zero inline row and
    drops the orphan external stage FILE timeseries so the run starts. With
    ``quality=True`` it also enables quality routing and appends a TSS
    pollutant carried by the DWF (Cdwf=500), so nodes hold a steady
    concentration that treatment edits can act on.
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
    name = "param_dwf.inp"
    if quality:
        txt = re.sub(r'^IGNORE_QUALITY\s+\S+', 'IGNORE_QUALITY       NO',
                     txt, count=1, flags=re.MULTILINE)
        txt += (
            "\n[POLLUTANTS]\n"
            ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
            "TSS MG/L 0.0 0.0 0.0 0.0 NO * 0.0 500.0 0.0\n"
        )
        name = "param_dwf_quality.inp"
    path = os.path.join(_OUT_DIR, name)
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
class TestLegacyPatternFactors(unittest.TestCase):
    def _biggest_dwf_node(self, s, nodes):
        idx = max(range(len(nodes)), key=lambda i: nodes[i].lateral_inflow)
        return idx, nodes[idx].lateral_inflow

    def test_pattern_factor_round_trip(self):
        """P6: set/get one pattern factor, plus read-only count/type."""
        s = _open(_derive_dwf_model(), "p6_legacy_rt")
        try:
            npat = s.get_object_count(_TP)
            self.assertGreater(npat, 0)
            count = int(s.get_value(_TP, _PatProp.COUNT, 0))
            self.assertGreater(count, 0)
            s.set_value(_TP, _PatProp.FACTOR, 0, 2.5, sub_index=0)
            self.assertAlmostEqual(
                s.get_value(_TP, _PatProp.FACTOR, 0, sub_index=0), 2.5, places=6)
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
            self.assertGreater(before, 1e-9, "expected a DWF-fed node")
            npat = s.get_object_count(_TP)
            for p in range(npat):
                cnt = int(s.get_value(_TP, _PatProp.COUNT, p))
                for k in range(cnt):
                    s.set_value(_TP, _PatProp.FACTOR, p, 10.0, sub_index=k)
            s.step()
            after = nodes[idx].lateral_inflow
            self.assertGreater(after, before * 1.5, (before, after))
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P4 — street sweeping (legacy parity)
# --------------------------------------------------------------------------- #
class TestLegacySweep(unittest.TestCase):
    def test_sweep_params_round_trip(self):
        """P4: set/get land-use sweep interval and removal mid-run."""
        s = _open(_SITE_DRAINAGE, "p4_legacy_rt")
        try:
            nlu = s.get_object_count(_LU)
            self.assertGreater(nlu, 0, "fixture must define land uses")
            for _ in range(5):
                s.step()
            s.set_value(_LU, _LuProp.SWEEP_INTERVAL, 0, 5.0)
            s.set_value(_LU, _LuProp.SWEEP_REMOVAL, 0, 0.65)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.SWEEP_INTERVAL, 0), 5.0, places=6)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.SWEEP_REMOVAL, 0), 0.65, places=6)
        finally:
            s.end(); s.finalize()

    def test_sweep_removal_bounds_rejected(self):
        """P4: an out-of-range removal fraction is rejected."""
        s = _open(_SITE_DRAINAGE, "p4_legacy_bounds")
        try:
            with self.assertRaises(Exception):
                s.set_value(_LU, _LuProp.SWEEP_REMOVAL, 0, 1.5)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P2 — buildup / washoff coefficients (legacy parity)
# --------------------------------------------------------------------------- #
class TestLegacyBuildupWashoff(unittest.TestCase):
    def test_buildup_coeffs_round_trip(self):
        """P2: set/get land-use buildup coefficients per pollutant (subIndex)."""
        s = _open(_SITE_DRAINAGE, "p2_legacy_buildup")
        try:
            self.assertGreater(s.get_object_count(_LU), 0)
            for _ in range(5):
                s.step()
            s.set_value(_LU, _LuProp.BUILDUP_COEFF1, 0, 77.0, sub_index=0)
            s.set_value(_LU, _LuProp.BUILDUP_COEFF2, 0, 0.4, sub_index=0)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.BUILDUP_COEFF1, 0, sub_index=0),
                77.0, places=6)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.BUILDUP_COEFF2, 0, sub_index=0),
                0.4, places=6)
        finally:
            s.end(); s.finalize()

    def test_washoff_coeffs_round_trip(self):
        """P2: set/get land-use washoff coefficients per pollutant (subIndex)."""
        s = _open(_SITE_DRAINAGE, "p2_legacy_washoff")
        try:
            for _ in range(5):
                s.step()
            # EMC washoff (funcType 3): concentration = coeff
            s.set_value(_LU, _LuProp.WASHOFF_FUNC, 0, 3, sub_index=0)
            s.set_value(_LU, _LuProp.WASHOFF_COEFF, 0, 250.0, sub_index=0)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.WASHOFF_FUNC, 0, sub_index=0),
                3, places=6)
            self.assertAlmostEqual(
                s.get_value(_LU, _LuProp.WASHOFF_COEFF, 0, sub_index=0),
                250.0, places=6)
        finally:
            s.end(); s.finalize()

    def test_washoff_effic_bounds_rejected(self):
        """P2: an out-of-range sweep efficiency is rejected."""
        s = _open(_SITE_DRAINAGE, "p2_legacy_bounds")
        try:
            with self.assertRaises(Exception):
                s.set_value(_LU, _LuProp.WASHOFF_SWEEP_EFFIC, 0, 2.0, sub_index=0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P5 — pollutant kinetics (legacy parity)
# --------------------------------------------------------------------------- #
_POBJ = solver.SWMMObjects.POLLUTANT
_PProp = solver.SWMMPollutantProperties


class TestLegacyKinetics(unittest.TestCase):
    def test_kinetics_round_trip(self):
        """P5: kdecay (1/day), co-fraction, snow-only set/get mid-run."""
        s = _open(_SITE_DRAINAGE, "p5_legacy_rt")
        try:
            for _ in range(5):
                s.step()
            s.set_value(_POBJ, _PProp.KDECAY, 0, 2.0)
            self.assertAlmostEqual(
                s.get_value(_POBJ, _PProp.KDECAY, 0), 2.0, places=6)
            s.set_value(_POBJ, _PProp.CO_FRACTION, 0, 0.3)
            self.assertAlmostEqual(
                s.get_value(_POBJ, _PProp.CO_FRACTION, 0), 0.3, places=6)
            s.set_value(_POBJ, _PProp.SNOW_ONLY, 0, 1)
            self.assertAlmostEqual(
                s.get_value(_POBJ, _PProp.SNOW_ONLY, 0), 1, places=6)
        finally:
            s.end(); s.finalize()

    def test_init_conc_rejected_running(self):
        """P5: init concentration is rejected mid-run (pre-start-only)."""
        s = _open(_SITE_DRAINAGE, "p5_legacy_initconc")
        try:
            for _ in range(5):
                s.step()
            with self.assertRaises(Exception):
                s.set_value(_POBJ, _PProp.INIT_CONCENTRATION, 0, 5.0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P3 — treatment expressions (legacy parity)
#
# swmm_setTreatment re-uses the [TREATMENT] input parser, freeing any prior
# equation first, so a mid-run set/replace/clear is leak-free and evaluated
# from the next routing step on (treatmnt_treat applies zero removal for
# cleared pairs).
# --------------------------------------------------------------------------- #
class TestLegacyTreatment(unittest.TestCase):
    def test_treatment_set_mid_run_reduces_quality(self):
        """P3: a mid-run "R = 0.95" treatment cuts node quality; clear recovers."""
        s = _open(_derive_dwf_model(quality=True), "p3_legacy_treat")
        try:
            nodes = LegacyNodes(s)
            # Step until a node carries the DWF-borne TSS (Cdwf=500).
            ni, before = -1, 0.0
            for _ in range(120):
                s.step()
                ni = max(range(len(nodes)),
                         key=lambda i: nodes[i].get_pollutant_concentration(0))
                before = nodes[ni].get_pollutant_concentration(0)
                if before > 10.0:
                    break
            self.assertGreater(before, 10.0, "expected a DWF-loaded node")
            s.set_treatment(ni, 0, "R = 0.95")
            for _ in range(2):
                s.step()
            treated = nodes[ni].get_pollutant_concentration(0)
            self.assertLess(treated, before * 0.5, (before, treated))
            # Clearing the expression lets quality recover.
            s.clear_treatment(ni, 0)
            for _ in range(4):
                s.step()
            recovered = nodes[ni].get_pollutant_concentration(0)
            self.assertGreater(recovered, treated, (treated, recovered))
        finally:
            s.end(); s.finalize()

    def test_treatment_accepts_name_and_rejects_garbage(self):
        """P3: node-name resolution works; an unparseable expression raises."""
        s = _open(_SITE_DRAINAGE, "p3_legacy_treat_err")
        try:
            for _ in range(3):
                s.step()
            name = s.get_object_name(solver.SWMMObjects.NODE, 0)
            s.set_treatment(name, 0, "R = 0.5")   # by name, parses fine
            with self.assertRaises(Exception):
                s.set_treatment(0, 0, "not a treatment expr")
            with self.assertRaises(Exception):
                s.set_treatment(len(LegacyNodes(s)), 0, "R = 0.5")  # bad index
            s.clear_treatment(name, 0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P11 — LID underdrain parameters (legacy parity)
#
# The drain coefficients are read live each routing step (lidproc.c), so
# swmm_setLidDrain is sound mid-run; surface/soil/storage layers stay an
# input-file concern (they seed unit state at start), matching the refactored
# pre-start-only guard.
# --------------------------------------------------------------------------- #
def _derive_lid_rb_model():
    """site_drainage + 10 rain barrels on S1 with a closed drain (coeff=0).

    The 2-yr storm ends by ~2 h; by 3 h the barrels hold water and J1 lateral
    inflow has receded to ~0.004 cfs, so a mid-run drain-coefficient edit is
    cleanly attributable.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_SITE_DRAINAGE) as f:
        txt = f.read()
    txt += (
        "\n[LID_CONTROLS]\n"
        ";;Name           Type/Layer Parameters\n"
        "RB1              RB\n"
        "RB1              STORAGE    36    0.75  0     0\n"
        "RB1              DRAIN      0     0.5   0     0     0     0\n"
        "\n[LID_USAGE]\n"
        ";;Subcatchment   LID Process      Number  Area     Width    InitSat  FromImp  ToPerv\n"
        "S1               RB1              10      100      0        0        50       0\n"
    )
    path = os.path.join(_OUT_DIR, "p11_lid_rb.inp")
    with open(path, "w") as f:
        f.write(txt)
    return path


class TestLegacyLidDrain(unittest.TestCase):
    def test_drain_edit_drains_barrels_mid_run(self):
        """P11: opening the underdrain mid-run produces outlet inflow next step."""
        s = _open(_derive_lid_rb_model(), "p11_legacy_drain")
        try:
            nodes = LegacyNodes(s)
            j1 = s.get_object_index(solver.SWMMObjects.NODE, "J1")
            while s.current_datetime.hour < 3:   # storm over, barrels full
                s.step()
            before = nodes[j1].lateral_inflow
            s.set_lid_drain("RB1", 10.0, 0.5, 0.0)
            for _ in range(4):
                s.step()
            after = nodes[j1].lateral_inflow
            self.assertGreater(after, max(before, 1e-3) * 10, (before, after))
        finally:
            s.end(); s.finalize()

    def test_drain_setter_bounds(self):
        """P11: bad index and negative coefficients are rejected."""
        s = _open(_derive_lid_rb_model(), "p11_legacy_drain_err")
        try:
            s.step()
            with self.assertRaises(Exception):
                s.set_lid_drain(99, 1.0, 0.5, 0.0)
            with self.assertRaises(Exception):
                s.set_lid_drain("RB1", -1.0, 0.5, 0.0)
        finally:
            s.end(); s.finalize()


# --------------------------------------------------------------------------- #
# P10 — aquifer parameters (legacy parity)
#
# Aquifer[] is re-read each step (gwater.c), so the flux-coefficient
# properties are settable while running with no cache; the structural /
# initial-condition properties bound or seed groundwater state and return
# ERR_API_IS_RUNNING mid-run, matching the refactored guard.
# --------------------------------------------------------------------------- #
_AQ = solver.SWMMObjects.AQUIFER
_AqProp = solver.SWMMAquiferProperties


class TestLegacyAquiferParams(unittest.TestCase):
    def test_flux_param_round_trip_mid_run(self):
        """P10: a flux-coefficient edit round-trips mid-run (input-file units)."""
        s = _open(_derive_dwf_model(), "p10_legacy_flux")
        try:
            self.assertGreater(s.get_object_count(_AQ), 0)
            for _ in range(5):
                s.step()
            k = s.get_value(_AQ, _AqProp.CONDUCTIVITY, 0)
            s.set_value(_AQ, _AqProp.CONDUCTIVITY, 0, k * 2.0 + 1.0)
            self.assertAlmostEqual(
                s.get_value(_AQ, _AqProp.CONDUCTIVITY, 0),
                k * 2.0 + 1.0, places=6)
            s.set_value(_AQ, _AqProp.UPPER_EVAP_FRAC, 0, 0.42)
            self.assertAlmostEqual(
                s.get_value(_AQ, _AqProp.UPPER_EVAP_FRAC, 0),
                0.42, places=6)
        finally:
            s.end(); s.finalize()

    def test_structural_param_rejected_running(self):
        """P10: structural params are rejected mid-run (pre-start-only)."""
        s = _open(_derive_dwf_model(), "p10_legacy_guard")
        try:
            for _ in range(5):
                s.step()
            with self.assertRaises(Exception):
                s.set_value(_AQ, _AqProp.POROSITY, 0, 0.4)
            with self.assertRaises(Exception):
                s.set_value(_AQ, _AqProp.WATER_TABLE_ELEV, 0, 10.0)
        finally:
            s.end(); s.finalize()

    def test_flux_param_bounds_rejected(self):
        """P10: out-of-range flux values are rejected."""
        s = _open(_derive_dwf_model(), "p10_legacy_bounds")
        try:
            s.step()
            with self.assertRaises(Exception):
                s.set_value(_AQ, _AqProp.CONDUCTIVITY, 0, -1.0)
            with self.assertRaises(Exception):
                s.set_value(_AQ, _AqProp.UPPER_EVAP_FRAC, 0, 2.0)
        finally:
            s.end(); s.finalize()
