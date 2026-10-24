"""A value written in display units reads back unchanged (2026-10-24).

The engine stores US units (ft, cfs) and fractions; setters convert from the
model's display units and getters convert back. The two roundings used to
leave a written value one ulp off (98.0 m read back as 97.99999999999999, and
1.7 % as 1.7000000000000002). Setters now store the internal value whose
display reads back exactly.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import os
import random
import re
import unittest

try:
    from openswmm.engine import Solver
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

# (collection, property): length, flow, area, rate and percent fields.
FIELDS = (
    ("nodes", "invert_elev"), ("nodes", "max_depth"), ("nodes", "surcharge_depth"),
    ("nodes", "ponded_area"), ("links", "length"), ("links", "offset_up"),
    ("links", "offset_dn"), ("links", "initial_flow"), ("links", "max_flow"),
    ("links", "seep_rate"), ("subcatchments", "width"), ("subcatchments", "slope"),
    ("subcatchments", "imperv_pct"), ("subcatchments", "zero_imperv_pct"),
)
VALUES = (98.0, 0.1, 1.7, 3.3, 123.456, 0.0254)


def _model(directory: str, flow_units: str) -> Solver:
    with open(SITE_DRAINAGE_INP, encoding="utf-8") as f:
        text = re.sub(r"(?m)^(FLOW_UNITS\s+)CFS", rf"\g<1>{flow_units}", f.read())
    inp = os.path.join(directory, f"model_{flow_units}.inp")
    with open(inp, "w", encoding="utf-8") as f:
        f.write(text)
    solver = Solver(inp, inp[:-4] + ".rpt", inp[:-4] + ".out")
    solver.open()
    return solver


class TestDisplayUnitRoundTrip(unittest.TestCase):
    def _check(self, flow_units: str, values) -> None:
        solver = _model(artifact_dir(self), flow_units)
        self.addCleanup(solver.close)
        for collection, prop in FIELDS:
            element = getattr(solver, collection)[0]
            for value in values:
                with self.subTest(units=flow_units, field=f"{collection}.{prop}", value=value):
                    setattr(element, prop, value)
                    self.assertEqual(getattr(element, prop), value)

    def test_si_lengths_flows_areas_and_percents(self):
        self._check("CMS", VALUES)
        self._check("LPS", VALUES)

    def test_us_percents(self):
        self._check("CFS", VALUES)

    def test_typical_authored_values(self):
        # Values with up to six significant digits, as decks and editors author them.
        rng = random.Random(20261024)
        values = [round(rng.uniform(0.001, 5000.0), rng.randint(0, 4)) for _ in range(200)]
        self._check("CMS", values)


if __name__ == "__main__":
    unittest.main()
