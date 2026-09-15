"""``OutputReader.pollutant_ids`` — the Python binding for
``swmm_output_get_pollut_id``.

Why the names matter, and why reading them by index is not enough: the
``.out`` per-column unit field is a three-value concentration enum with no
HOURS slot, so the water-age column (``__WATER_AGE__``, reported in hours)
necessarily reuses a concentration code. A consumer that keys on the unit
code sees two MG/L columns, one of which is hours. The NAME is the only
discriminator, and until ``swmm_output_get_pollut_id`` landed (``06580dd6``)
nothing in the modern stack could read it — Python exposed
``pollutant_count`` and nothing else.

Gates here:

* ``test_water_age_deck_names_both_species`` — the ordered-equality
  assertion ``["TSS", "__WATER_AGE__"]``. Order is asserted, not just
  membership: a header whose name list disagrees with the data column order
  passes every value-by-index check while telling a consumer the wrong
  thing.
* ``test_reference_deck_names_its_pollutant`` — the no-water-age path on the
  shared reference model, so the binding is not shown to work only on a deck
  written by this file.
* ``test_pollutant_ids_returns_a_copy`` — the property caches; it must hand
  back a copy, or one caller's mutation silently rewrites every later read.

Fixtures are written to the reviewable per-test artifact directory
(CLAUDE.md 4.1).
"""

from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._output_reader  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import OutputReader, Solver

from tests._paths import SITE_DRAINAGE_INP, artifact_dir


#: Level pool: wet junctions, a FIXED outfall at the water surface, so there
#: is no flow and no transport physics enters any assertion here. The deck
#: mirrors ``tests/unit/engine/test_output_quality.cpp``'s, which is where
#: the C-level gates for the same reader live.
_DECK = """[TITLE]
python species-id gate deck

[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:10:00
ROUTING_STEP 5
REPORT_STEP 00:01:00
WATER_AGE ON
QUALITY_SOLVER EULERIAN_ARD

[JUNCTIONS]
J0 10.0 10 1.5 0 0
J1 10.0 10 1.5 0 0
J2 10.0 10 1.5 0 0

[OUTFALLS]
OUT 10.0 FIXED 11.5 NO

[CONDUITS]
C1 J0 J1 500 0.013 0 0 0
C2 J1 J2 500 0.013 0 0 0
C3 J2 OUT 500 0.013 0 0 0

[XSECTIONS]
C1 CIRCULAR 2.0 0 0 0
C2 CIRCULAR 2.0 0 0 0
C3 CIRCULAR 2.0 0 0 0

[POLLUTANTS]
;;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit
TSS    MG/L  0     0   0     0      NO       *        0      0    42

[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.waterage config="age.cfg"

[REPORT]
INPUT NO
"""

_AGE_CFG = "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n"


class SpeciesIdCase(unittest.TestCase):
    """Runs a deck to completion and re-opens the ``.out`` it wrote."""

    def _run(self, inp: str) -> str:
        d = artifact_dir(self)
        rpt = os.path.join(d, "model.rpt")
        out = os.path.join(d, "model.out")
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        for _ in s.steps():
            pass
        s.end()
        s.report()
        s.close()
        s.destroy()
        self.assertTrue(os.path.exists(out), f"no .out written to {out}")
        return out

    def _reader(self, out_path: str) -> OutputReader:
        o = OutputReader(out_path)
        self.addCleanup(o.close)
        return o


class TestSpeciesIds(SpeciesIdCase):
    def test_water_age_deck_names_both_species(self):
        d = artifact_dir(self)
        inp = os.path.join(d, "age.inp")
        with open(os.path.join(d, "age.cfg"), "w", encoding="utf-8") as fh:
            fh.write(_AGE_CFG)
        with open(inp, "w", encoding="utf-8") as fh:
            fh.write(_DECK)

        out = self._reader(self._run(inp))

        # Setup assertion before the result one: if the deck did not actually
        # enable water age, the equality below would fail for a reason that
        # has nothing to do with the binding under test.
        self.assertEqual(
            out.pollutant_count,
            2,
            "the deck did not produce two species columns — WATER_AGE never "
            "reached the writer, so this gate is not testing the binding",
        )
        self.assertEqual(out.pollutant_ids, ["TSS", "__WATER_AGE__"])

    def test_reference_deck_names_its_pollutant(self):
        out = self._reader(self._run(SITE_DRAINAGE_INP))
        self.assertEqual(out.pollutant_ids, ["TSS"])
        self.assertEqual(len(out.pollutant_ids), out.pollutant_count)

    def test_pollutant_ids_returns_a_copy(self):
        out = self._reader(self._run(SITE_DRAINAGE_INP))
        first = out.pollutant_ids
        first.append("MUTATED")
        self.assertEqual(out.pollutant_ids, ["TSS"])


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
