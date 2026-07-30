"""Node->cell triangle-coupling rows through the Python bindings.

Pins the contract of the four `[2D_TRIANGLE_NODE_MAP]` row bindings
added to ``Surface2D``: ``add_triangle_coupling`` (append semantics),
``triangle_coupling_rows``, ``get_triangle_coupling_row`` and
``clear_triangle_couplings``. Rows authored in the ``.inp`` read back;
runtime-added rows append (a triangle may carry several rows); clearing
removes every row and resets the legacy per-triangle mirror; invalid
rows (bad triangle, empty name, non-positive cd/area) raise.

Outputs land in ``tests/_artifacts/<test id>/`` for review (project
convention — no hidden temp files).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import Solver

from ._paths import artifact_dir

# Two-triangle patch coupled to junction J1 — same base model as
# tests/test_2d_marcher_options.py, with an {extra_sections} slot for
# an authored [2D_TRIANGLE_NODE_MAP].
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

[2D_VERTICES]
 0.0    0.0   1.0
10.0    0.0   1.0
10.0   10.0   1.0
 0.0   10.0   1.0

[2D_TRIANGLES]
0     1   2   0.03
0     2   3   0.03
{extra_sections}
"""


class TwoDTriangleCouplingTest(unittest.TestCase):
    def _open_model(self, name: str, extra_sections: str = "") -> Solver:
        d = artifact_dir(self)
        inp = os.path.join(d, name + ".inp")
        with open(inp, "w") as f:
            f.write(_MODEL.format(extra_sections=extra_sections))
        s = Solver(inp, inp.replace(".inp", ".rpt"),
                   inp.replace(".inp", ".out"))
        s.open()
        self.addCleanup(s.close)
        return s

    def test_authored_rows_read_back(self):
        s = self._open_model(
            "authored",
            "\n[2D_TRIANGLE_NODE_MAP]\n"
            "0         J1    0.7   2.0\n",
        )
        surf = s.surface2d
        self.assertEqual(surf.triangle_coupling_rows, 1)
        tri, node, cd, area = surf.get_triangle_coupling_row(0)
        self.assertEqual(tri, 0)
        self.assertGreaterEqual(node, 0)  # J1 resolved
        self.assertAlmostEqual(cd, 0.7)
        self.assertAlmostEqual(area, 2.0)

    def test_add_appends_rows(self):
        s = self._open_model("append")
        surf = s.surface2d
        self.assertEqual(surf.triangle_coupling_rows, 0)
        surf.add_triangle_coupling(0, "J1", 0.65, 1.0)
        surf.add_triangle_coupling(0, "J1", 0.5, 2.5)  # same triangle: appends
        surf.add_triangle_coupling(1, "J1", 0.8, 3.0)
        self.assertEqual(surf.triangle_coupling_rows, 3)
        rows = [surf.get_triangle_coupling_row(i) for i in range(3)]
        self.assertEqual([r[0] for r in rows], [0, 0, 1])
        self.assertAlmostEqual(rows[1][2], 0.5)
        self.assertAlmostEqual(rows[1][3], 2.5)

    def test_clear_removes_all_rows(self):
        s = self._open_model("clear")
        surf = s.surface2d
        surf.add_triangle_coupling(0, "J1", 0.65, 1.0)
        surf.add_triangle_coupling(1, "J1", 0.65, 1.0)
        self.assertEqual(surf.triangle_coupling_rows, 2)
        surf.clear_triangle_couplings()
        self.assertEqual(surf.triangle_coupling_rows, 0)
        # Legacy per-triangle mirror is reset too.
        self.assertEqual(surf.get_triangle_coupled_node(0), -1)
        self.assertEqual(surf.get_triangle_coupled_node(1), -1)

    def test_invalid_rows_raise(self):
        s = self._open_model("invalid")
        surf = s.surface2d
        with self.assertRaises(RuntimeError):  # bad triangle index
            surf.add_triangle_coupling(99, "J1", 0.65, 1.0)
        with self.assertRaises(RuntimeError):  # empty node name
            surf.add_triangle_coupling(0, "", 0.65, 1.0)
        with self.assertRaises(RuntimeError):  # non-positive cd
            surf.add_triangle_coupling(0, "J1", 0.0, 1.0)
        with self.assertRaises(RuntimeError):  # non-positive area
            surf.add_triangle_coupling(0, "J1", 0.65, -1.0)
        self.assertEqual(surf.triangle_coupling_rows, 0)


if __name__ == "__main__":
    unittest.main()
