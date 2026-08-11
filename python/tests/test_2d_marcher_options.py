"""[2D_OPTIONS] explicit-marcher keys through the Python bindings.

The marcher configuration (INTEGRATOR, THETA, CFL_NUMBER, LTS_TIERS,
H_MOVE, FROUDE_MAX, COUPLING_AREA) is exposed through the generic
``solver.options.ext`` mapping — the same pass-through every other
[2D_OPTIONS] key uses. These tests pin the round-trip contract: defaults
read back, writes stick, invalid values raise, and a short coupled run
under INTEGRATOR EXPLICIT completes with a closed 2D ledger.

Outputs land in ``tests/_artifacts/<test id>/`` for review (project
convention — no hidden temp files).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver
from openswmm.engine import EngineError

from ._paths import artifact_dir

# Two-triangle patch coupled to junction J1 — the same model the engine's
# C++ marcher fixtures use (tests/unit/engine/test_2d_junction_coupling.cpp),
# shortened to 10 simulated minutes.
_MODEL = """\
[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             00:10:00
REPORT_STEP          00:01:00
ROUTING_STEP         1
ALLOW_PONDING        NO

[JUNCTIONS]
J1      0.0   1.0       0          0         0

[OUTFALLS]
O1     -0.5    FREE  NO

[CONDUITS]
C1      J1    O1  30.0    0.013      0         0          0

[XSECTIONS]
C1      CIRCULAR  0.2    0      0      0      1

[INFLOWS]
J1      FLOW         IN_TS    FLOW  1.0      1.0

[TIMESERIES]
IN_TS   0:00   0.10
IN_TS   0:08   0.10
IN_TS   0:09   0.0

[2D_OPTIONS]
MAX_TIMESTEP     1
DRY_DEPTH        0.002
COUPLING_CD      0.7
REPORT_2D        NO
{extra}

[2D_VERTICES]
 0.0    0.0   1.0
10.0    0.0   1.0
10.0   10.0   1.0
 0.0   10.0   1.0

[2D_TRIANGLES]
0     1   2   0.03
0     2   3   0.03

[2D_VERTEX_NODE_MAP]
0         J1    0.7  1.0
"""

_MARCHER_KEYS_DEFAULTS = {
    # EXPLICIT is the default — and only — 2D integrator (D2 retirement of the
    # CVODE/ARKODE stack, 2026-07-29); no INTEGRATOR line is required.
    "INTEGRATOR": "EXPLICIT",
    "THETA": 0.8,
    "CFL_NUMBER": 0.7,
    "LTS_TIERS": 4,
    "H_MOVE": 0.003,
    "FROUDE_MAX": 1.5,
    "ADVECTION": "NO",
    "COUPLING_AREA": "DEFAULT",
}


class TwoDMarcherOptionsTest(unittest.TestCase):
    def _write_model(self, name: str, extra: str = "") -> tuple[str, str, str]:
        d = artifact_dir(self)
        inp = os.path.join(d, name + ".inp")
        with open(inp, "w") as f:
            f.write(_MODEL.format(extra=extra))
        return inp, inp.replace(".inp", ".rpt"), inp.replace(".inp", ".out")

    def test_defaults_read_back(self):
        inp, rpt, out = self._write_model("defaults")
        s = Solver(inp, rpt, out)
        s.open()
        try:
            ext = s.options.ext
            for key, want in _MARCHER_KEYS_DEFAULTS.items():
                got = ext[key]
                if isinstance(want, str):
                    self.assertEqual(got.upper(), want, key)
                elif isinstance(want, int):
                    self.assertEqual(int(got), want, key)
                else:
                    self.assertAlmostEqual(float(got), want, msg=key)
        finally:
            s.close()

    def test_inp_keys_parse_and_read_back(self):
        inp, rpt, out = self._write_model(
            "authored",
            "INTEGRATOR       EXPLICIT\n"
            "THETA            0.9\n"
            "CFL_NUMBER       0.5\n"
            "LTS_TIERS        6\n"
            "H_MOVE           0.005\n"
            "FROUDE_MAX       2.0\n"
            "ADVECTION        YES\n"
            "COUPLING_AREA    AUTO\n",
        )
        s = Solver(inp, rpt, out)
        s.open()
        try:
            ext = s.options.ext
            self.assertEqual(ext["INTEGRATOR"].upper(), "EXPLICIT")
            self.assertAlmostEqual(float(ext["THETA"]), 0.9)
            self.assertAlmostEqual(float(ext["CFL_NUMBER"]), 0.5)
            self.assertEqual(int(ext["LTS_TIERS"]), 6)
            self.assertAlmostEqual(float(ext["H_MOVE"]), 0.005)
            self.assertAlmostEqual(float(ext["FROUDE_MAX"]), 2.0)
            self.assertEqual(ext["ADVECTION"].upper(), "YES")
            self.assertEqual(ext["COUPLING_AREA"].upper(), "AUTO")
        finally:
            s.close()

    def test_set_round_trip(self):
        inp, rpt, out = self._write_model("set_roundtrip")
        s = Solver(inp, rpt, out)
        s.open()
        try:
            ext = s.options.ext
            ext["INTEGRATOR"] = "EXPLICIT"
            ext["THETA"] = 0.85
            ext["CFL_NUMBER"] = 0.6
            ext["LTS_TIERS"] = 5
            ext["H_MOVE"] = 0.004
            ext["FROUDE_MAX"] = 1.2
            ext["ADVECTION"] = "YES"
            ext["COUPLING_AREA"] = "AUTO"
            self.assertEqual(ext["INTEGRATOR"].upper(), "EXPLICIT")
            self.assertAlmostEqual(float(ext["THETA"]), 0.85)
            self.assertAlmostEqual(float(ext["CFL_NUMBER"]), 0.6)
            self.assertEqual(int(ext["LTS_TIERS"]), 5)
            self.assertAlmostEqual(float(ext["H_MOVE"]), 0.004)
            self.assertAlmostEqual(float(ext["FROUDE_MAX"]), 1.2)
            self.assertEqual(ext["ADVECTION"].upper(), "YES")
            self.assertEqual(ext["COUPLING_AREA"].upper(), "AUTO")
        finally:
            s.close()

    def test_invalid_values_raise(self):
        inp, rpt, out = self._write_model("invalid")
        s = Solver(inp, rpt, out)
        s.open()
        try:
            ext = s.options.ext
            for key, bad in (
                ("INTEGRATOR", "RK4"),
                # Retired with the CVODE/ARKODE stack (D2) — hard errors now.
                ("INTEGRATOR", "CVODE"),
                ("INTEGRATOR", "ARKODE"),
                ("LINEAR_SOLVER", "GMRES"),
                ("MAX_CVODE_STEPS", "500"),
                ("COUPLING_WINDOW", "30"),
                ("THETA", "1.5"),
                ("CFL_NUMBER", "0"),
                ("LTS_TIERS", "9"),
                ("FROUDE_MAX", "-1"),
                ("ADVECTION", "MAYBE"),
                ("COUPLING_AREA", "MAYBE"),
            ):
                with self.assertRaises(EngineError, msg=f"{key}={bad}"):
                    ext[key] = bad
        finally:
            s.close()

    def test_explicit_run_completes(self):
        inp, rpt, out = self._write_model(
            "explicit_run", "INTEGRATOR       EXPLICIT\n")
        s = Solver(inp, rpt, out)
        s.open()
        try:
            s.initialize()
            s.start()
            for _ in s.steps():
                pass
            s.end()
            s.report()
        finally:
            s.close()
        with open(rpt) as f:
            txt = f.read()
        self.assertIn("2D Surface Routing Continuity", txt)
        # Marcher telemetry rows (the window/CVODE stats retired with D2).
        self.assertIn("Internal Steps", txt)
        self.assertIn("LTS Tier 0 Occupancy", txt)
        # Engine error lines are "  ERROR nnn:" / "ERROR:" at line start —
        # distinct from the "Error-Test Failures" statistics row label.
        for line in txt.splitlines():
            self.assertFalse(line.strip().startswith("ERROR"),
                             f"engine error in report: {line.strip()}")


if __name__ == "__main__":
    unittest.main()
