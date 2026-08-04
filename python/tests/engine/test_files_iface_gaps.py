"""[FILES] RUNOFF and RDII interface slots (USE/SAVE) through the Solver.

End-to-end coverage of plans/FILES_INTERFACE_GAP_CLOSURE_PLAN_2026-07-02.md:
SAVE RUNOFF/RDII export interface files from the [FILES] slots; USE replays
them, with USE RDII overriding the internal unit-hydrograph computation.

Scratch files live in ``python/tests/engine/output/`` so they can be
reviewed after a run (transparent file IO per CLAUDE.md §4.1).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver, MassBalance, RoutingTotal

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")

_OPTIONS = """[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
INFILTRATION         HORTON
START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/01/2026
END_TIME             02:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:00:30
"""

_NETWORK = """
[JUNCTIONS]
J1  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
O1  95.0  FREE

[CONDUITS]
C1  J1  O1  400.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1

[TIMESERIES]
TS1  0:00  1.0
TS1  1:00  0.0
"""

_RAIN_RUNOFF = """
[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[SUBCATCHMENTS]
S1  RG1  J1  10.0  100.0  500.0  1.0  0

[SUBAREAS]
S1  0.012  0.1  0.05  0.05  100.0  OUTLET

[INFILTRATION]
S1  3.0  0.5  4.0  7.0  0
"""

_RDII = """
[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[HYDROGRAPHS]
UH1  RG1
UH1  ALL  SHORT   0.30  1.0  2.0  0  0  0
UH1  ALL  MEDIUM  0.20  2.0  4.0  0  0  0
UH1  ALL  LONG    0.10  4.0  8.0  0  0  0

[RDII]
J1  UH1  5.0
"""


def _run(tag: str, inp_text: str) -> Solver:
    inp = os.path.join(OUTPUT_DIR, f"gaps_{tag}.inp")
    with open(inp, "w") as f:
        f.write(inp_text)
    s = Solver(
        inp,
        os.path.join(OUTPUT_DIR, f"gaps_{tag}.rpt"),
        os.path.join(OUTPUT_DIR, f"gaps_{tag}.out"),
    )
    s.open()
    s.initialize()
    s.start()
    for _ in s.steps():
        pass
    s.end()
    return s


def _total(solver: Solver, component) -> float:
    return MassBalance(solver).routing_total(component)


class TestRunoffSlot(unittest.TestCase):
    def test_save_then_use_reproduces_wet_weather(self):
        rof = os.path.join(OUTPUT_DIR, "gaps_runoff.rof")

        save_inp = (_OPTIONS + _RAIN_RUNOFF + _NETWORK
                    + f'\n[FILES]\nSAVE RUNOFF "{rof}"\n')
        s = _run("rof_save", save_inp)
        try:
            wet_save = _total(s, RoutingTotal.WET_WEATHER)
        finally:
            s.close()
            s.destroy()
        self.assertGreater(wet_save, 0.0)
        self.assertTrue(os.path.exists(rof))

        use_inp = (_OPTIONS + _RAIN_RUNOFF + _NETWORK
                   + f'\n[FILES]\nUSE RUNOFF "{rof}"\n')
        s = _run("rof_use", use_inp)
        try:
            wet_use = _total(s, RoutingTotal.WET_WEATHER)
        finally:
            s.close()
            s.destroy()
        self.assertAlmostEqual(wet_use, wet_save, delta=abs(wet_save) * 0.05)


class TestRdiiSlot(unittest.TestCase):
    def test_save_then_use_conserves_rdii(self):
        rdf = os.path.join(OUTPUT_DIR, "gaps_rdii.rdf")

        # Network-first section order, matching the C++ twin. (Any order
        # works — [RDII] node names are re-resolved post-parse like legacy
        # two-pass parsing; test_section_order_parity.cpp pins that.)
        save_inp = (_OPTIONS + _NETWORK + _RDII
                    + f'\n[FILES]\nSAVE RDII "{rdf}"\n')
        s = _run("rdii_save", save_inp)
        try:
            rdii_save = _total(s, RoutingTotal.RDII)
        finally:
            s.close()
            s.destroy()
        self.assertGreater(rdii_save, 0.0)
        with open(rdf, "rb") as f:
            self.assertEqual(f.read(10), b"SWMM5-RDII")

        use_inp = (_OPTIONS + _NETWORK + _RDII
                   + f'\n[FILES]\nUSE RDII "{rdf}"\n')
        s = _run("rdii_use", use_inp)
        try:
            rdii_use = _total(s, RoutingTotal.RDII)
        finally:
            s.close()
            s.destroy()
        self.assertGreater(rdii_use, 0.0)
        self.assertAlmostEqual(rdii_use, rdii_save,
                               delta=abs(rdii_save) * 0.15)

    def test_missing_use_file_fails_start(self):
        inp_text = (_OPTIONS + _NETWORK + _RDII
                    + '\n[FILES]\nUSE RDII "no_such_rdii.rdf"\n')
        inp = os.path.join(OUTPUT_DIR, "gaps_rdii_missing.inp")
        with open(inp, "w") as f:
            f.write(inp_text)
        s = Solver(
            inp,
            os.path.join(OUTPUT_DIR, "gaps_rdii_missing.rpt"),
            os.path.join(OUTPUT_DIR, "gaps_rdii_missing.out"),
        )
        s.open()
        s.initialize()
        try:
            with self.assertRaises(Exception):
                s.start()
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()
