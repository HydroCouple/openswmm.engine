"""Iteration-4 quality-editing round-trips (Python parity for the new C APIs).

Covers:
  * ``subcatchment.loadings`` — the [LOADINGS] initial-buildup mapping
    (parse → read, set → get).
  * ``subcatchment.coverages()`` — bulk coverage percents.
  * ``Landuse.rename`` / ``Pollutants.rename`` — in-place renames that keep
    positional data and follow name-stored references.
  * ``Pollutants.add`` on an OPENED model — grow-preserving matrices.
  * ``quality.validate_treatment_expression`` — non-mutating validation
    with message + column.

Report/output files land in ``tests/engine/output``.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")


def _quality_model(tag):
    text = (
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         KINWAVE\n"
        "INFILTRATION         HORTON\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             02:00:00\n"
        "REPORT_STEP          00:05:00\n"
        "ROUTING_STEP         0:00:30\n"
        "\n[RAINGAGES]\n"
        "RG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
        "\n[SUBCATCHMENTS]\n"
        "S1  RG1  J1  10.0  50  500  0.5  100\n"
        "S2  RG1  J1  5.0   50  400  0.5  0\n"
        "\n[SUBAREAS]\n"
        "S1  0.01  0.1  0.05  0.05  25  OUTLET\n"
        "S2  0.01  0.1  0.05  0.05  25  OUTLET\n"
        "\n[INFILTRATION]\n"
        "S1  3.0  0.5  4.0  7  0\n"
        "S2  3.0  0.5  4.0  7  0\n"
        "\n[TIMESERIES]\n"
        "TS1  01/01/2026 00:00 1.0\n"
        "TS1  01/01/2026 01:00 0.0\n"
        "\n[JUNCTIONS]\n"
        "J1  100.0  10.0  0.0  0.0  0.0\n"
        "\n[OUTFALLS]\n"
        "O1  95.0  FREE\n"
        "\n[CONDUITS]\n"
        "C1  J1  O1  400.0  0.013  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0  1\n"
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS   MG/L  10  1  2  0.1  NO  *    0.0   3  4\n"
        "Lead  UG/L  0   0  0  0    NO  TSS  0.25  0  0\n"
        "\n[LANDUSES]\n"
        "Res  7   0.5  2\n"
        "Com  14  0.3  0\n"
        "\n[COVERAGES]\n"
        "S1  Res  60\n"
        "S1  Com  40\n"
        "S2  Res  25\n"
        "\n[LOADINGS]\n"
        "S1  TSS  1.5\n"
        "S2  Lead  2.25\n"
        "\n[BUILDUP]\n"
        "Res  TSS  POW  100  2  1.5  AREA\n"
        "\n[WASHOFF]\n"
        "Res  TSS  EXP  0.1  1.2  30  15\n"
        "\n[DWF]\n"
        "J1  Lead  0.5\n"
    )
    os.makedirs(_OUT_DIR, exist_ok=True)
    path = os.path.join(_OUT_DIR, f"quality_edit_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


import contextlib


@contextlib.contextmanager
def _open(tag):
    # Editing APIs require the OPENED state, so open WITHOUT the Solver
    # context manager (which auto-initializes and starts the simulation).
    base = os.path.join(_OUT_DIR, f"quality_edit_{tag}")
    s = Solver(_quality_model(tag), base + ".rpt", base + ".out")
    s.open()
    try:
        yield s
    finally:
        s.close()
        s.destroy()


class TestQualityEditing(unittest.TestCase):

    def test_loadings_mapping(self):
        with _open("loadings") as s:
            s1 = s.subcatchments["S1"]
            s2 = s.subcatchments["S2"]
            # Parsed [LOADINGS] rows are visible (previously a parse no-op).
            self.assertAlmostEqual(s1.loadings["TSS"], 1.5)
            self.assertAlmostEqual(s2.loadings["Lead"], 2.25)
            self.assertAlmostEqual(s1.loadings["Lead"], 0.0)
            # Set → get.
            s2.loadings["TSS"] = 7.5
            self.assertAlmostEqual(s2.loadings["TSS"], 7.5)
            self.assertEqual(set(s1.loadings), {"TSS"})

    def test_bulk_coverages(self):
        with _open("coverages") as s:
            s1 = s.subcatchments["S1"]
            cov = s1.coverages()
            lus = list(s.quality.landuses)
            by_id = {lu.id: cov[lu.index] for lu in lus}
            self.assertAlmostEqual(by_id["Res"], 60.0)
            self.assertAlmostEqual(by_id["Com"], 40.0)
            # Mapping view agrees.
            self.assertAlmostEqual(s1.coverage["Res"], 60.0)

    def test_landuse_rename_keeps_data(self):
        with _open("lurename") as s:
            lu = s.quality.landuses["Res"]
            lu.rename("Residential")
            self.assertEqual(
                s.quality.landuses.get_index("Residential"), lu.index)
            self.assertAlmostEqual(lu.sweep_interval, 7.0)
            bu = s.quality.get_buildup("Residential", "TSS")
            self.assertEqual(int(bu["func"]), 1)
            self.assertAlmostEqual(
                s.subcatchments["S1"].coverage["Residential"], 60.0)

    def test_pollutant_rename_and_add_preserve(self):
        with _open("polrename") as s:
            s.pollutants.rename("Lead", "Pb")
            self.assertIn("Pb", s.pollutants)
            self.assertNotIn("Lead", s.pollutants)

            # Add on an OPENED model (previously LIFECYCLE-rejected) and
            # confirm the buildup matrix survived the stride change.
            s.pollutants.add("BOD")
            bu = s.quality.get_buildup("Res", "TSS")
            self.assertEqual(int(bu["func"]), 1)
            self.assertAlmostEqual(float(bu["c1"]), 100.0)
            wo = s.quality.get_washoff("Res", "TSS")
            self.assertEqual(int(wo["func"]), 1)
            self.assertAlmostEqual(
                s.subcatchments["S1"].loadings["TSS"], 1.5)

    def test_validate_treatment_expression(self):
        with _open("validate") as s:
            ok, msg, col = s.quality.validate_treatment_expression(
                "R = 1.0 - exp(-0.5 * HRT)")
            self.assertTrue(ok)
            self.assertEqual(msg, "")

            ok, msg, col = s.quality.validate_treatment_expression(
                "C = FLOW * 2")
            self.assertFalse(ok)
            self.assertIn("FLOW", msg)
            self.assertEqual(col, 4)

            ok, msg, col = s.quality.validate_treatment_expression("X = 1")
            self.assertFalse(ok)
            self.assertTrue(msg)

            # Non-mutating: no treatment materialized.
            self.assertEqual(s.quality.get_treatment("J1", "TSS"), "")


if __name__ == "__main__":
    unittest.main()
