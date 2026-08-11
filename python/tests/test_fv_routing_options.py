"""``FLOW_ROUTING FV`` and the ``FV_*`` option family through the bindings.

The finite-volume solver's configuration is exposed through the generic
``solver.options`` mapping, which passes straight through to the engine's
option API. That API rejects keys it does not recognize, so a key known
only to the ``[OPTIONS]`` file parser would be invisible here — these
tests pin the round trip that keeps the bindings, the MCP server and the
GUI in step with the parser.

They also run the solver: a scheme that parses its options and then
produces nothing would pass every round-trip assertion.

Outputs land in ``tests/_artifacts/<test id>/`` for review (project
convention — no hidden temp files).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import EngineError, RouteModel, Solver

from ._paths import artifact_dir

# A single sloping conduit between a junction and a free outfall, with a
# steady external inflow. Sized so the routed volume is several acre-feet:
# the report prints volumes to three decimals, so a model routing only a
# hundredth of an acre-foot cannot support a continuity assertion tighter
# than its own display rounding.
_MODEL = """\
[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         FV
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             02:00:00
REPORT_STEP          00:01:00
ROUTING_STEP         5
ALLOW_PONDING        NO

[JUNCTIONS]
J1      10.0   4.0       0          0         0

[OUTFALLS]
O1       8.0   FREE  NO

[CONDUITS]
C1      J1     O1     400   0.013  0  0  0

[XSECTIONS]
C1      CIRCULAR   3.0   0  0  0  1

[INFLOWS]
J1      FLOW   ""     FLOW   1.0   1.0   20.0

[TIMESERIES]

[REPORT]
INPUT      NO
CONTROLS   NO
"""


def _write_model(test_case) -> tuple[str, str, str]:
    d = artifact_dir(test_case)
    inp = os.path.join(d, "model.inp")
    with open(inp, "w", encoding="utf-8") as fh:
        fh.write(_MODEL)
    return inp, os.path.join(d, "model.rpt"), os.path.join(d, "model.out")


class FvRoutingOptions(unittest.TestCase):
    def test_route_model_enum_carries_fv(self):
        self.assertEqual(int(RouteModel.FV), 3)
        self.assertEqual(RouteModel(3), RouteModel.FV)

    def test_defaults_read_back(self):
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            self.assertEqual(s.options["FLOW_ROUTING"], "FV")
            self.assertEqual(s.options["FV_RIEMANN"], "HLLC")
            self.assertEqual(s.options["FV_LIMITER"], "MINMOD")
            self.assertEqual(s.options["FV_SCALAR_SCHEME"], "MUSCL")
            self.assertEqual(s.options["FV_TIME_INTEGRATION"], "EULER")
            self.assertEqual(s.options["FV_BACKEND"], "AUTO")
            self.assertEqual(s.options["FV_COMPACTION"], "YES")
            self.assertEqual(int(s.options["FV_ORDER"]), 1)
            self.assertEqual(int(s.options["FV_MIN_CELLS"]), 4)
            self.assertAlmostEqual(float(s.options["FV_CELL_LENGTH"]), 0.0)
            self.assertAlmostEqual(float(s.options["FV_CFL"]), 0.5)
            self.assertAlmostEqual(float(s.options["FV_SLOT_CELERITY"]), 100.0)
        finally:
            s.close()

    def test_every_key_round_trips(self):
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            numeric = {
                "FV_CELL_LENGTH": 12.5,
                "FV_CFL": 0.35,
                "FV_SLOT_CELERITY": 250.0,
                "FV_DISPERSION": 4.0,
            }
            exact = {
                "FV_MIN_CELLS": "3",
                "FV_ORDER": "2",
                "FV_RIEMANN": "HLL",
                "FV_LIMITER": "SUPERBEE",
                "FV_SCALAR_SCHEME": "QUICKEST_ULTIMATE",
                "FV_TIME_INTEGRATION": "RK2",
                "FV_STRUCTURE_COUPLING": "ROUTING_STEP",
                "FV_COMPACTION": "NO",
                "FV_BACKEND": "CPU",
                "FV_MIN_PARALLEL_CELLS": "12345",
                "FV_NODE_COUPLING": "EXPLICIT",
                "FV_NODE_DT": "NONE",
                "FV_NODE_PICARD": "3",
                "FV_LTS": "NO",
                "FV_LTS_MAX_TIERS": "5",
                "FV_CFL_CENSUS_INTERVAL": "10",
            }
            for k, v in numeric.items():
                s.options[k] = str(v)
            for k, v in exact.items():
                s.options[k] = v

            for k, v in numeric.items():
                self.assertAlmostEqual(float(s.options[k]), v, places=6, msg=k)
            for k, v in exact.items():
                self.assertEqual(s.options[k], v, msg=k)
        finally:
            s.close()

    def test_all_fv_keys_are_iterable(self):
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            seen = {k for k in s.options if k.startswith("FV_")}
        finally:
            s.close()
        self.assertEqual(
            seen,
            {
                "FV_CELL_LENGTH", "FV_MIN_CELLS", "FV_CFL", "FV_RIEMANN",
                "FV_ORDER", "FV_LIMITER", "FV_SCALAR_SCHEME",
                "FV_TIME_INTEGRATION", "FV_SLOT_CELERITY", "FV_DISPERSION",
                "FV_STRUCTURE_COUPLING", "FV_COMPACTION", "FV_BACKEND",
                "FV_MIN_PARALLEL_CELLS",
                "FV_LTS", "FV_LTS_MAX_TIERS", "FV_CFL_CENSUS_INTERVAL",
                "FV_NODE_COUPLING", "FV_NODE_DT", "FV_NODE_PICARD",
            },
        )

    def test_invalid_enum_value_is_rejected(self):
        # A silent no-op here would leave a user believing they had selected a
        # scheme they had not.
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            for key, bad in (("FV_RIEMANN", "ROE"),
                             ("FV_LIMITER", "NONESUCH"),
                             ("FV_SCALAR_SCHEME", "WENO"),
                             ("FV_BACKEND", "METAL")):
                with self.assertRaises(EngineError, msg=key):
                    s.options[key] = bad
        finally:
            s.close()

    def test_fv_keys_are_inert_under_dynwave(self):
        # Plan §4.2: switching FLOW_ROUTING must never invalidate a file.
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            s.options["FLOW_ROUTING"] = "DYNWAVE"
            s.options["FV_CELL_LENGTH"] = "5"
            self.assertAlmostEqual(float(s.options["FV_CELL_LENGTH"]), 5.0)
            self.assertEqual(s.options["FLOW_ROUTING"], "DYNWAVE")
        finally:
            s.close()

    def test_fv_run_completes_and_routes_water(self):
        inp, rpt, out = _write_model(self)
        s = Solver(inp, rpt, out)
        s.open()
        try:
            s.options["FV_CELL_LENGTH"] = "20"
            s.initialize()
            s.start()
            for _ in s.steps():
                pass
            s.end()
            s.report()
        finally:
            s.close()

        with open(rpt, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("Flow Routing Continuity", text)

        # The conservative form is the reason this solver exists, so hold it to
        # the claim: routing continuity within a tenth of a percent.
        block = text.split("Flow Routing Continuity", 1)[1]
        line = next(ln for ln in block.splitlines() if "Continuity Error" in ln)
        # The report pads with a run of dots AND the value carries a decimal
        # point, so take the last whitespace-separated token rather than
        # splitting on ".".
        err = float(line.rsplit(maxsplit=1)[-1])
        self.assertLess(abs(err), 0.1, f"routing continuity error {err} %")


if __name__ == "__main__":
    unittest.main()
