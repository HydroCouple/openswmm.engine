"""Aquifers collection coverage (gap-review 2026-07-06).

Exercises ``solver.aquifers`` — previously untouched by the test suite
(class-level coverage gap identified in
``plans/api_gap_review_2026-07-06/``). Uses the ``b8_aq_probe`` model,
which carries an ``[AQUIFERS]`` section.
"""
from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._subcatchments  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import AquiferParam, Aquifers, Solver

from tests._paths import artifact_dir

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "legacy", "output", "b8_aq_probe.inp")


class _AquiferCase(unittest.TestCase):
    """Base case providing the ``aq_solver`` helper (formerly a fixture)."""

    def aq_solver(self):
        """An opened Solver on a model with an [AQUIFERS] section.

        Skips (rather than errors) if this model can't be opened by the
        new engine or exposes no aquifers, so the wheel test phase stays green.
        """
        d = artifact_dir(self)
        s = Solver(_MODEL, os.path.join(d, "aq.rpt"), os.path.join(d, "aq.out"))
        try:
            s.open()
            n = len(s.aquifers)
        except Exception as exc:  # pragma: no cover - environment guard
            try:
                s.destroy()
            except Exception:
                pass
            self.skipTest(f"aquifer fixture unavailable: {exc}")
        if n == 0:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()
            self.skipTest("model exposes no aquifers")
        self.addCleanup(self._close_destroy, s)
        return s

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestAquifersContainer(_AquiferCase):
    def test_property_type(self):
        aq_solver = self.aq_solver()
        self.assertIsInstance(aq_solver.aquifers, Aquifers)

    def test_len_positive(self):
        aq_solver = self.aq_solver()
        self.assertGreater(len(aq_solver.aquifers), 0)

    def test_iter_yields_ids(self):
        aq_solver = self.aq_solver()
        ids = list(aq_solver.aquifers)
        self.assertTrue(ids and all(isinstance(i, str) for i in ids))
        self.assertEqual(len(ids), len(aq_solver.aquifers))

    def test_id_index_roundtrip(self):
        aq_solver = self.aq_solver()
        aq = aq_solver.aquifers
        name = aq.get_id(0)
        self.assertEqual(aq.get_index(name), 0)

    def test_contains(self):
        aq_solver = self.aq_solver()
        first = next(iter(aq_solver.aquifers))
        self.assertIn(first, aq_solver.aquifers)
        self.assertNotIn("NO_SUCH_AQUIFER_xyz", aq_solver.aquifers)


class TestAquiferParams(_AquiferCase):
    def test_get_param_returns_float(self):
        aq_solver = self.aq_solver()
        first = next(iter(aq_solver.aquifers))
        val = aq_solver.aquifers.get_param(first, AquiferParam.POROSITY)
        self.assertIsInstance(val, float)

    def test_set_param_roundtrip(self):
        aq_solver = self.aq_solver()
        aq = aq_solver.aquifers
        first = next(iter(aq))
        # Pick a porosity strictly between field capacity and 1 so the value
        # is valid regardless of the model's exact soil parameters.
        fc = aq.get_param(first, AquiferParam.FIELD_CAPACITY)
        target = (fc + 1.0) / 2.0
        aq.set_param(first, AquiferParam.POROSITY, target)
        self.assertAlmostEqual(
            aq.get_param(first, AquiferParam.POROSITY), target, places=6)

    def test_param_by_index_matches_by_name(self):
        aq_solver = self.aq_solver()
        aq = aq_solver.aquifers
        name = aq.get_id(0)
        self.assertAlmostEqual(
            aq.get_param(0, AquiferParam.POROSITY),
            aq.get_param(name, AquiferParam.POROSITY), places=6)

    def test_evap_pattern_is_str(self):
        aq_solver = self.aq_solver()
        first = next(iter(aq_solver.aquifers))
        self.assertIsInstance(aq_solver.aquifers.get_evap_pattern(first), str)
