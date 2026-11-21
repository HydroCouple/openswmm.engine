"""Per-triangle initial depth and velocity through the Python bindings.

Pins the contract of the ``[2D_TRIANGLES]``/``[2D_INITIAL_VELOCITY]``/
``[2D_INITIAL_HGL]`` initial-condition bindings on ``Surface2D``:
``get_triangle_init_depth`` / ``set_triangle_init_depth``,
``get_triangle_init_velocity`` / ``set_triangle_init_velocity`` and
``get_triangle_init_hgl`` / ``set_triangle_init_hgl``. Values authored in the
``.inp`` read back; runtime writes round-trip; invalid values raise.

Outputs land in ``tests/_artifacts/<test id>/`` for review (project
convention -- no hidden temp files).
"""

from __future__ import annotations

import math
import os
import unittest

from openswmm.engine import BadParamError, BadIndexError, Solver

from ._paths import artifact_dir

# Two-triangle patch -- same base model as
# tests/test_2d_triangle_coupling.py, with an {extra_sections} slot and a
# {triangle_rows} slot so a test can author INIT_DEPTH inline.
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
{triangle_rows}
{extra_sections}
"""

# MANNINGS_N only -- INIT_DEPTH defaults to 0 (dry).
_DRY_ROWS = "0     1   2   0.03\n0     2   3   0.03"

# MANNINGS_N + INIT_DEPTH; the depth column is written before TAG.
_WET_ROWS = "0     1   2   0.03   0.25\n0     2   3   0.03   0.50"


class TwoDTriangleInitialStateTest(unittest.TestCase):
    def _open_model(self, name: str, *, triangle_rows: str = _DRY_ROWS,
                    extra_sections: str = "") -> Solver:
        d = artifact_dir(self)
        inp = os.path.join(d, name + ".inp")
        with open(inp, "w") as f:
            f.write(_MODEL.format(triangle_rows=triangle_rows,
                                  extra_sections=extra_sections))
        s = Solver(inp, inp.replace(".inp", ".rpt"),
                   inp.replace(".inp", ".out"))
        s.open()
        self.addCleanup(s.close)
        return s

    # -- depth ---------------------------------------------------------

    def test_depth_defaults_to_dry(self):
        surf = self._open_model("depth_default").surface2d
        self.assertAlmostEqual(surf.get_triangle_init_depth(0), 0.0)
        self.assertAlmostEqual(surf.get_triangle_init_depth(1), 0.0)

    def test_authored_depth_reads_back(self):
        surf = self._open_model("depth_authored",
                                triangle_rows=_WET_ROWS).surface2d
        self.assertAlmostEqual(surf.get_triangle_init_depth(0), 0.25)
        self.assertAlmostEqual(surf.get_triangle_init_depth(1), 0.50)

    def test_depth_round_trips(self):
        surf = self._open_model("depth_roundtrip").surface2d
        surf.set_triangle_init_depth(0, 0.4)
        surf.set_triangle_init_depth(1, 1.25)
        self.assertAlmostEqual(surf.get_triangle_init_depth(0), 0.4)
        self.assertAlmostEqual(surf.get_triangle_init_depth(1), 1.25)
        # Zero is a legal value -- it means "start dry".
        surf.set_triangle_init_depth(0, 0.0)
        self.assertAlmostEqual(surf.get_triangle_init_depth(0), 0.0)

    def test_invalid_depth_raises(self):
        surf = self._open_model("depth_invalid").surface2d
        with self.assertRaises(BadParamError):  # negative depth
            surf.set_triangle_init_depth(0, -0.1)
        with self.assertRaises(BadIndexError):  # bad triangle index
            surf.set_triangle_init_depth(99, 0.1)
        self.assertAlmostEqual(surf.get_triangle_init_depth(0), 0.0)

    # -- water-surface elevation (HGL) ----------------------------------

    def test_hgl_defaults_to_unset(self):
        surf = self._open_model("hgl_default").surface2d
        self.assertTrue(math.isnan(surf.get_triangle_init_hgl(0)))
        self.assertTrue(math.isnan(surf.get_triangle_init_hgl(1)))

    def test_authored_hgl_reads_back(self):
        surf = self._open_model(
            "hgl_authored",
            extra_sections="[2D_INITIAL_HGL]\n*  1.75\n1  0.5\n").surface2d
        self.assertAlmostEqual(surf.get_triangle_init_hgl(0), 1.75)
        # Below the bed (z = 1) is legal: the cell starts dry.
        self.assertAlmostEqual(surf.get_triangle_init_hgl(1), 0.5)

    def test_hgl_round_trips_and_clears(self):
        surf = self._open_model("hgl_roundtrip").surface2d
        surf.set_triangle_init_hgl(0, 2.5)
        self.assertAlmostEqual(surf.get_triangle_init_hgl(0), 2.5)
        self.assertTrue(math.isnan(surf.get_triangle_init_hgl(1)))
        surf.set_triangle_init_hgl(0, math.nan)  # NaN clears it
        self.assertTrue(math.isnan(surf.get_triangle_init_hgl(0)))

    def test_hgl_overrides_depth_and_clamps_at_the_bed(self):
        # Flat bed at z = 1: cell 0 at HGL 1.4 holds 0.4 m regardless of its
        # INIT_DEPTH; cell 1 at HGL 0.5 (below the bed) starts dry.
        s = self._open_model(
            "hgl_seeds", triangle_rows=_WET_ROWS,
            extra_sections="[2D_INITIAL_HGL]\n0  1.4\n1  0.5\n")
        s.initialize()
        s.start()
        depths = s.surface2d.get_depths()
        self.assertAlmostEqual(depths[0], 0.4, places=9)
        self.assertEqual(depths[1], 0.0)

    def test_invalid_hgl_raises(self):
        surf = self._open_model("hgl_invalid").surface2d
        with self.assertRaises(BadParamError):  # infinite elevation
            surf.set_triangle_init_hgl(0, math.inf)
        with self.assertRaises(BadIndexError):  # bad triangle index
            surf.set_triangle_init_hgl(99, 1.0)
        self.assertTrue(math.isnan(surf.get_triangle_init_hgl(0)))

    # -- velocity ------------------------------------------------------

    def test_velocity_defaults_to_rest(self):
        surf = self._open_model("vel_default").surface2d
        self.assertEqual(surf.get_triangle_init_velocity(0), (0.0, 0.0))

    def test_velocity_round_trips(self):
        surf = self._open_model("vel_roundtrip").surface2d
        surf.set_triangle_init_velocity(0, 1.5, -0.75)
        u, v = surf.get_triangle_init_velocity(0)
        self.assertAlmostEqual(u, 1.5)
        self.assertAlmostEqual(v, -0.75)
        # Triangle 1 is untouched.
        self.assertEqual(surf.get_triangle_init_velocity(1), (0.0, 0.0))

    def test_invalid_velocity_raises(self):
        surf = self._open_model("vel_invalid").surface2d
        with self.assertRaises(BadParamError):  # non-finite u
            surf.set_triangle_init_velocity(0, math.inf, 0.0)
        with self.assertRaises(BadParamError):  # non-finite v
            surf.set_triangle_init_velocity(0, 0.0, math.nan)
        with self.assertRaises(BadIndexError):  # bad triangle index
            surf.set_triangle_init_velocity(99, 1.0, 1.0)
        self.assertEqual(surf.get_triangle_init_velocity(0), (0.0, 0.0))


if __name__ == "__main__":
    unittest.main()
