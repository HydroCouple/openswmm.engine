"""Initial node depth and link flow are authored values (2026-10-24).

``Node.initial_depth`` wrote the current depth and ``Link.initial_flow`` the
current flow, while initialization starts the run from the authored values
(``[JUNCTIONS]`` InitDepth, ``[CONDUITS]`` InitFlow) and the .inp writer saves
those. A value set before initialization was lost, and ``initial_flow`` read
the current flow rather than the deck's. Both now set and read the authored
value; after initialization the start state already exists, so setting
raises ``LifecycleError``.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import os
import re
import unittest

try:
    from openswmm.engine import LifecycleError, Solver
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir


def _deck(directory: str) -> str:
    """The example deck with conduit C1 given an authored initial flow of 0.5 cfs."""
    with open(SITE_DRAINAGE_INP, encoding="utf-8") as f:
        text = f.read()
    start = text.index("[CONDUITS]")
    # Link  From  To  Length  Roughness  InOffset  OutOffset  InitFlow  MaxFlow
    text = text[:start] + re.sub(r"(?m)^(C1(?:\s+\S+){6}\s+)0\b", r"\g<1>0.5", text[start:], count=1)
    inp = os.path.join(directory, "model.inp")
    with open(inp, "w", encoding="utf-8") as f:
        f.write(text)
    return inp


def _open(inp: str) -> Solver:
    solver = Solver(inp, inp[:-4] + ".rpt", inp[:-4] + ".out")
    solver.open()
    return solver


class TestInitialConditions(unittest.TestCase):
    def setUp(self):
        self.out = artifact_dir(self)
        self.solver = _open(_deck(self.out))
        self.addCleanup(self.solver.close)
        self.node, self.link = self.solver.nodes["J1"], self.solver.links["C1"]

    def test_initial_flow_reads_the_deck(self):
        self.assertEqual(self.link.initial_flow, 0.5)

    def test_values_set_before_initialize_start_the_run(self):
        self.node.initial_depth, self.link.initial_flow = 1.25, 2.0
        self.assertEqual((self.node.initial_depth, self.link.initial_flow), (1.25, 2.0))
        self.solver.initialize()
        self.assertEqual(self.node.depth, 1.25)
        self.assertEqual(self.link.flow, 2.0)
        self.assertEqual((self.node.initial_depth, self.link.initial_flow), (1.25, 2.0))

    def test_values_are_saved_with_the_model(self):
        self.node.initial_depth, self.link.initial_flow = 1.25, 2.0
        saved = os.path.join(self.out, "saved.inp")
        self.solver.write(saved)
        reopened = _open(saved)
        self.addCleanup(reopened.close)
        self.assertEqual(reopened.nodes["J1"].initial_depth, 1.25)
        self.assertEqual(reopened.links["C1"].initial_flow, 2.0)

    def test_setting_after_initialize_is_refused(self):
        self.solver.initialize()
        with self.assertRaises(LifecycleError):
            self.node.initial_depth = 1.0
        with self.assertRaises(LifecycleError):
            self.link.initial_flow = 1.0
        self.assertEqual(self.link.initial_flow, 0.5)  # the run does not change it


if __name__ == "__main__":
    unittest.main()
