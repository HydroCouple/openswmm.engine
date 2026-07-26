"""``Solver.set_lenient_open`` and the ``open_errors`` / ``open_warnings``
accumulators — bindings for ``swmm_engine_set_lenient_open`` and
``swmm_get_error_count/at`` / ``swmm_get_warning_count/at``.

A clean model opened either strictly or leniently exposes empty/­list-valued
accumulators; a deliberately broken model opened leniently stays ``OPENED``
(so the objects remain inspectable) and records at least one error instead of
failing the open. The broken-model fixture is written to the reviewable
per-test artifact directory (CLAUDE.md §4.1).
"""

import os
import unittest

from openswmm.engine import Solver, EngineState, EngineError

from tests._paths import SITE_DRAINAGE_INP, artifact_dir


class TestLenientOpenAccumulators(unittest.TestCase):
    def _solver(self, inp):
        d = artifact_dir(self)
        return Solver(
            inp,
            os.path.join(d, "model.rpt"),
            os.path.join(d, "model.out"),
        )

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()

    # -- clean model -------------------------------------------------------

    def test_clean_model_has_list_accumulators(self):
        s = self._solver(SITE_DRAINAGE_INP)
        self.addCleanup(self._close_destroy, s)
        s.set_lenient_open(True)
        s.open()
        self.assertIsInstance(s.open_errors, list)
        self.assertIsInstance(s.open_warnings, list)
        # A valid reference model records no post-parse validation errors.
        self.assertEqual(s.open_errors, [])

    def test_lenient_toggle_off_is_accepted(self):
        s = self._solver(SITE_DRAINAGE_INP)
        self.addCleanup(self._close_destroy, s)
        s.set_lenient_open(True)
        s.set_lenient_open(False)  # back to strict; must still open cleanly
        s.open()
        self.assertEqual(s.state, EngineState.OPENED)

    def test_default_argument_enables(self):
        s = self._solver(SITE_DRAINAGE_INP)
        self.addCleanup(self._close_destroy, s)
        s.set_lenient_open()  # defaults to True
        s.open()
        self.assertEqual(s.state, EngineState.OPENED)

    # -- broken model ------------------------------------------------------

    def _write_broken_inp(self):
        """A model whose subcatchment drains to an undefined outlet node —
        ERROR 209, raised by post-parse cross-reference resolution rather than
        by the reader. (An undefined node named in [CONDUITS] is *not* usable
        here: the reader accepts it silently, recording no error at all.)"""
        d = artifact_dir(self)
        path = os.path.join(d, "broken.inp")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(
                "[TITLE]\nLenient open fixture\n\n"
                "[OPTIONS]\n"
                "FLOW_UNITS           CFS\n"
                "START_DATE           01/01/2020\n"
                "END_DATE             01/01/2020\n"
                "\n"
                "[JUNCTIONS]\n"
                ";;Name  Elev  MaxDepth\n"
                "J1       0     4\n"
                "\n"
                "[OUTFALLS]\n"
                ";;Name  Elev  Type\n"
                "O1       0     FREE\n"
                "\n"
                "[CONDUITS]\n"
                ";;Name  From  To   Length  Rough  In  Out\n"
                "C1       J1    O1   100     0.01   0   0\n"
                "\n"
                "[XSECTIONS]\n"
                ";;Link  Shape     G1  G2  G3  G4\n"
                "C1       CIRCULAR  1   0   0   0\n"
                "\n"
                "[SUBCATCHMENTS]\n"
                ";;Name  Rgage  Outlet        Area  %Imperv  Width  Slope  CurbLen\n"
                "S1       RG1    UNDEFINED_ND  1     50       100    0.5    0\n"
                "\n"
                "[SUBAREAS]\n"
                ";;Subcatch  N-Imperv  N-Perv  S-Imperv  S-Perv  PctZero  RouteTo\n"
                "S1          0.01      0.1     0.05      0.05    25       OUTLET\n"
                "\n"
                "[INFILTRATION]\n"
                ";;Subcatch  P1    P2    P3\n"
                "S1          3.0   0.5   4\n"
                "\n"
                "[RAINGAGES]\n"
                ";;Name  Format     Interval  SCF  Source\n"
                "RG1      INTENSITY  1:00      1.0  TIMESERIES TS1\n"
                "\n"
                "[TIMESERIES]\n"
                "TS1  01/01/2020  00:00  0.0\n"
            )
        return path

    def test_broken_model_lenient_open_records_errors(self):
        s = self._solver(self._write_broken_inp())
        self.addCleanup(self._close_destroy, s)
        s.set_lenient_open(True)
        s.open()
        self.assertEqual(s.state, EngineState.OPENED)
        self.assertIsInstance(s.open_errors, list)
        self.assertGreaterEqual(len(s.open_errors), 1)
        self.assertIn("209", " ".join(s.open_errors))

    def test_broken_model_strict_open_still_fails(self):
        """The same fixture must fail a strict open — otherwise the lenient
        assertion above proves nothing about the flag."""
        s = self._solver(self._write_broken_inp())
        self.addCleanup(self._close_destroy, s)
        with self.assertRaises(EngineError):
            s.open()


if __name__ == "__main__":
    unittest.main()
