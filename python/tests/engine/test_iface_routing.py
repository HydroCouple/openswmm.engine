"""Routing interface files ([FILES] USE INFLOWS / SAVE OUTFLOWS).

End-to-end coverage of the lifecycle wiring added per
``plans/ROUTING_INTERFACE_FILE_INTEGRATION_PLAN_2026-07-01.md``: a model
with ``USE INFLOWS`` receives the file's flows as external inflows, a model
with ``SAVE OUTFLOWS`` writes a legacy-format interface file, and a chained
A→B pair conserves volume at the shared boundary.

All scratch files live in ``python/tests/engine/output/`` so they can be
reviewed after a run (transparent file IO per CLAUDE.md §4.1).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver, MassBalance
from openswmm.engine import RoutingTotal

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")

FLOW_CFS = 2.0
SIM_SECONDS = 3600.0


def _model_inp(receiver: str, outfall: str, files_line: str = "") -> str:
    """Minimal 1-junction / 1-outfall model, optionally with a [FILES] row."""
    return f"""[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/01/2026
END_TIME             01:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:00:30

[JUNCTIONS]
{receiver}  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
{outfall}  95.0  FREE

[CONDUITS]
C1  {receiver}  {outfall}  400.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1

{files_line}
"""


def _iface_file(node: str, flow_cfs: float) -> str:
    """Legacy-format interface file with a constant flow over 3 h."""
    return (
        "SWMM5 Interface File\n"
        "test inflows\n"
        "300  - reporting time step in sec\n"
        "1    - number of constituents as listed below:\n"
        "FLOW CFS\n"
        "1    - number of nodes as listed below:\n"
        f"{node}\n"
        "Node             Year Mon Day Hr  Min Sec FLOW\n"
        f"{node}  2026 01 01 00 00 00 {flow_cfs}\n"
        f"{node}  2026 01 01 03 00 00 {flow_cfs}\n"
    )


def _write(path: str, content: str) -> str:
    with open(path, "w") as f:
        f.write(content)
    return path


def _run(tag: str, inp_text: str) -> Solver:
    inp = _write(os.path.join(OUTPUT_DIR, f"iface_{tag}.inp"), inp_text)
    s = Solver(
        inp,
        os.path.join(OUTPUT_DIR, f"iface_{tag}.rpt"),
        os.path.join(OUTPUT_DIR, f"iface_{tag}.out"),
    )
    s.open()
    s.initialize()
    s.start()
    for _ in s.steps():
        pass
    s.end()
    return s


class TestUseInflows(unittest.TestCase):
    def test_inflow_file_drives_external_inflow(self):
        iface = _write(
            os.path.join(OUTPUT_DIR, "iface_use_in.txt"),
            _iface_file("J1", FLOW_CFS),
        )
        s = _run("use", _model_inp("J1", "O1",
                                   f'[FILES]\nUSE INFLOWS "{iface}"\n'))
        try:
            ext = MassBalance(s).routing_total(RoutingTotal.EXTERNAL)
            expected = FLOW_CFS * SIM_SECONDS  # 7200 ft³
            self.assertAlmostEqual(ext, expected, delta=abs(expected) * 0.10)
        finally:
            s.close()
            s.destroy()

    def test_missing_inflow_file_fails_start(self):
        inp_text = _model_inp(
            "J1", "O1", '[FILES]\nUSE INFLOWS "no_such_iface_file.txt"\n'
        )
        inp = _write(os.path.join(OUTPUT_DIR, "iface_missing.inp"), inp_text)
        s = Solver(
            inp,
            os.path.join(OUTPUT_DIR, "iface_missing.rpt"),
            os.path.join(OUTPUT_DIR, "iface_missing.out"),
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


class TestSaveOutflowsAndChaining(unittest.TestCase):
    def test_chained_models_conserve_boundary_volume(self):
        a_in = _write(
            os.path.join(OUTPUT_DIR, "iface_a_in.txt"),
            _iface_file("J1", FLOW_CFS),
        )
        a_out = os.path.join(OUTPUT_DIR, "iface_a_out.txt")

        # Model A: interface inflow at J1 → outfall O1, saved to a_out.
        s_a = _run(
            "chain_a",
            _model_inp(
                "J1", "O1",
                f'[FILES]\nUSE INFLOWS "{a_in}"\nSAVE OUTFLOWS "{a_out}"\n',
            ),
        )
        try:
            a_outflow = MassBalance(s_a).routing_total(RoutingTotal.OUTFLOW)
        finally:
            s_a.close()
            s_a.destroy()
        self.assertGreater(a_outflow, 0.0)

        # The saved file must be a legacy-format interface file.
        with open(a_out) as f:
            content = f.read()
        self.assertTrue(content.startswith("SWMM5 Interface File"))
        self.assertIn("FLOW CFS", content)
        self.assertIn("O1", content)

        # Model B: junction named O1 receives model A's outfall rows.
        s_b = _run(
            "chain_b",
            _model_inp("O1", "OB", f'[FILES]\nUSE INFLOWS "{a_out}"\n'),
        )
        try:
            b_ext = MassBalance(s_b).routing_total(RoutingTotal.EXTERNAL)
        finally:
            s_b.close()
            s_b.destroy()

        self.assertGreater(b_ext, 0.0)
        self.assertAlmostEqual(b_ext, a_outflow, delta=abs(a_outflow) * 0.15)
