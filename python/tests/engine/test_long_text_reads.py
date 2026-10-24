"""Long text survives binding reads (2026-10-24).

Native text getters NUL-terminate and silently truncate to the caller's
buffer. The bindings read them through growing buffers, so text longer than
the former fixed ``char[128]``/``char[256]`` buffers comes back whole,
including multi-byte UTF-8 at the boundaries. ``TestFixedBuffers`` in
``tests/test_api_coverage.py`` keeps new fixed buffers out.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import os
import unittest

try:
    from openswmm.engine import Solver, UserFlagType
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

# 3-byte UTF-8 characters, so a cut inside a character would fail to decode.
LONG = "雨水" * 300  # 1,800 bytes
ID_LONG = "series_" + "é" * 200  # names are tokens: no spaces


class TestLongTextReads(unittest.TestCase):
    def setUp(self):
        out = artifact_dir(self)
        self.solver = Solver(SITE_DRAINAGE_INP, os.path.join(out, "model.rpt"),
                             os.path.join(out, "model.out"))
        self.solver.open()
        self.addCleanup(self.solver.close)

    def test_element_tags(self):
        node, link, sub = self.solver.nodes[0], self.solver.links[0], self.solver.subcatchments[0]
        node.tag, link.tag, sub.tag = LONG, LONG + "L", LONG + "S"
        self.assertEqual(node.tag, LONG)
        self.assertEqual(link.tag, LONG + "L")
        self.assertEqual(sub.tag, LONG + "S")

    def test_external_inflow_names(self):
        inflows = self.solver.inflows
        self.solver.tables.add_timeseries(ID_LONG)
        before = inflows.external_count
        inflows.add_external(0, "FLOW", ts_name=ID_LONG)
        row = inflows.get_external(before)
        self.assertEqual(row[2], ID_LONG)

    def test_user_flag_text(self):
        flags = self.solver.userflags
        flags.define("NOTE", UserFlagType.STRING, LONG)
        definition = next(d for d in flags.definitions() if d.name == "NOTE")
        self.assertEqual(definition.description, LONG)
        node = self.solver.nodes[0].id
        flags.set_value("NODE", node, "NOTE", LONG)
        self.assertEqual(flags.get_value("NODE", node, "NOTE"), LONG)


if __name__ == "__main__":
    unittest.main()
