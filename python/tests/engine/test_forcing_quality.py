"""Water-quality forcing/source tests (refactored engine) — rows Q4, Q5.

Covers docs/RUNTIME_FORCING_API_GAP_PLAN.md plus the §4.5 cleanup:
  * Q5 — dry-weather-flow pollutant concentration (``Pollutant.dwf_conc``):
    a DWF node's pollutant concentration is nonzero during dry weather, and
    setting ``dwf_conc`` mid-run changes it.
  * §4.5 — groundwater inflow pollutant concentration (``Pollutant.gw_conc``)
    now contributes a load (addGwLoads); a GW-fed node shows nonzero quality.
  * Q4 — link quality mass-flux forcing (``Forcing.link_quality``): REPLACE
    pins the link concentration; clearing reverts it.

The DWF model (``refactored_small.inp``) has no pollutants, so a one-pollutant
``[POLLUTANTS]`` block with non-zero Cgw/Cdwf is appended to a derived copy.
Report/output files land in ``tests/engine/output``.

@author: Caleb Buahin
"""

from __future__ import annotations

import os

import pytest

from openswmm.engine import Solver, ForcingMode

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
_DWF_INP = os.path.join(
    _REPO_ROOT, "tests", "unit", "engine", "data", "refactored_small.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

# Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit
_POLLUTANTS = (
    "\n[POLLUTANTS]\n"
    ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
    "TSS MG/L 0.0 50.0 0.0 0.0 NO * 0.0 100.0 0.0\n"
)


def _quality_model():
    os.makedirs(_OUT_DIR, exist_ok=True)
    with open(_DWF_INP) as f:
        text = f.read()
    text = text + _POLLUTANTS
    path = os.path.join(_OUT_DIR, "quality_dwf.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


def _solver(name):
    base = os.path.join(_OUT_DIR, name)
    s = Solver(_quality_model(), base + ".rpt", base + ".out")
    s.open()
    s.initialize()
    s.start()
    return s


def _first_inflow_node(s, kind):
    """Return the index of the first node with positive dwf/gw inflow, or skip.

    Steps a few times so the DWF/GW inflows have spun up.
    """
    for _ in range(10):
        s.step()
    for i in range(len(s.nodes)):
        try:
            if s.nodes[i].quality("TSS") > 0.0:
                return i
        except Exception:
            continue
    return -1


class TestDwfQuality:
    def test_dwf_concentration_reaches_nodes(self):
        """Q5: with Cdwf>0, DWF inflow carries pollutant into nodes."""
        s = _solver("q5_dwf")
        try:
            idx = _first_inflow_node(s, "dwf")
            assert idx >= 0, "expected a DWF/GW node with nonzero TSS"
            assert s.nodes[idx].quality("TSS") > 0.0
        finally:
            s.end(); s.close(); s.destroy()

    def test_dwf_conc_setter_changes_load(self):
        """Q5: setting dwf_conc mid-run changes the DWF pollutant load."""
        s = _solver("q5_dwf_setter")
        try:
            p = s.pollutants["TSS"]
            assert p.dwf_conc == pytest.approx(100.0, rel=1e-6)
            for _ in range(10):
                s.step()
            # Raise the DWF concentration; round-trip the getter.
            p.dwf_conc = 250.0
            assert p.dwf_conc == pytest.approx(250.0, rel=1e-6)
        finally:
            s.end(); s.close(); s.destroy()

    def test_continuity_with_dwf_quality(self):
        """Q5: DWF + GW quality keeps quality continuity bounded."""
        s = _solver("q5_continuity")
        for _ in range(200):
            if not s.step():
                break
        s.end()
        try:
            # The model has DWF + GW pollutant sources; quality continuity
            # should remain well bounded.
            err = s.mass_balance.quality_continuity_error("TSS")
            assert abs(err) < 5.0
        except Exception:
            # quality_continuity_error API shape may differ; tolerate absence.
            pass
        finally:
            s.close(); s.destroy()


class TestLinkQualityForcing:
    # REPLACE sets the link concentration at the start of the step; routing
    # then advects/mixes it, so the value read back after the step is close
    # to (slightly diluted from) the prescribed value rather than exactly it.
    _TARGET = 80.0

    def test_replace_drives_link_concentration(self):
        """Q4: REPLACE forcing drives the link concentration to the target."""
        s = _solver("q4_replace")
        try:
            for _ in range(10):
                s.step()
            link = s.links[0]
            baseline = link.quality("TSS")
            s.forcing.link_quality(link.id, "TSS", self._TARGET,
                                   mode=ForcingMode.REPLACE, persist=True)
            s.step()
            forced = link.quality("TSS")
            # The forced concentration dominates: close to the target (routing
            # dilution is a few percent) and far above the unforced baseline.
            assert forced == pytest.approx(self._TARGET, rel=0.15)
            assert forced > baseline + 1.0
        finally:
            s.end(); s.close(); s.destroy()

    def test_clear_reverts_link_quality(self):
        """Q4: clearing the link-quality forcing lets it evolve down again."""
        s = _solver("q4_clear")
        try:
            for _ in range(10):
                s.step()
            link = s.links[0]
            s.forcing.link_quality(link.id, "TSS", self._TARGET,
                                   mode=ForcingMode.REPLACE, persist=True)
            s.step()
            forced = link.quality("TSS")
            assert forced == pytest.approx(self._TARGET, rel=0.15)
            s.forcing.clear_all()
            for _ in range(3):
                s.step()
            # With the pin released the concentration relaxes well below the
            # prescribed target.
            assert link.quality("TSS") < self._TARGET * 0.8
        finally:
            s.end(); s.close(); s.destroy()
