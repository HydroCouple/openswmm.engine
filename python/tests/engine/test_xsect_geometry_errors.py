"""Cross-section geometry errors name what is wrong (2026-10-24).

EPA SWMM reports an unusable cross section as ``ERROR 211: invalid number``
with an empty token and leans on the line number it prints. The engine prints
no line number for these, so the message read ``ERROR 211: invalid number .``
It now names the value, the column and the link, or, for a shape whose
dimensions do not fit together, the link and its geometry.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import os
import re
import unittest

try:
    from openswmm.engine import ParseError, Solver
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir


def _with_xsection(directory: str, row: str) -> str:
    """The example deck with conduit C1's [XSECTIONS] row replaced by ``row``."""
    with open(SITE_DRAINAGE_INP, encoding="utf-8") as f:
        text = f.read()
    section = text.index("[XSECTIONS]")
    text = text[:section] + re.sub(r"(?m)^C1\s+.*$", row, text[section:], count=1)
    inp = os.path.join(directory, "model.inp")
    with open(inp, "w", encoding="utf-8") as f:
        f.write(text)
    return inp


class TestXsectGeometryErrors(unittest.TestCase):
    def _open_error(self, row: str) -> str:
        inp = _with_xsection(artifact_dir(self), row)
        solver = Solver(inp, inp[:-4] + ".rpt", inp[:-4] + ".out")
        with self.assertRaises(ParseError) as caught:
            solver.open()
        solver.close()
        with open(inp[:-4] + ".rpt", encoding="utf-8", errors="replace") as f:
            self.assertIn(str(caught.exception).strip(), f.read())
        return str(caught.exception)

    def test_zero_depth_names_value_column_and_link(self):
        for value in ("0", "-1.5", "0.0"):
            with self.subTest(geom1=value):
                message = self._open_error(f"C1  TRAPEZOIDAL  {value}  5  5  5  1")
                self.assertIn(
                    f"ERROR 211: invalid number {value} for Geom1 of link C1 in [XSECTIONS] "
                    "(must be > 0).", message)

    def test_inconsistent_shape_names_the_link_and_its_geometry(self):
        # A filled circular pipe filled to above its crown.
        message = self._open_error("C1  FILLED_CIRCULAR  1  1.2  0  0  1")
        self.assertIn(
            "ERROR 211: invalid number in the cross-section geometry of link C1 "
            "(Geom1-4: 1 1.2 0 0).", message)


if __name__ == "__main__":
    unittest.main()
