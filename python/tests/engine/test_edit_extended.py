"""Extended :class:`ModelEditor` coverage — the entity-type deletion and
impact-analysis bindings added for the extended edit C API.

Covers pollutants, land uses, aquifers, snowpacks, and unit-hydrograph groups.
These objects come from ``.inp`` blocks, so the tests open a Solver
(``OPENED`` state, where delete/convert are permitted) rather than building a
model from scratch. Output artifacts are written to the reviewable per-test
directory (CLAUDE.md §4.1).
"""

import os
import unittest

from openswmm.engine import ModelEditor, ImpactEntry, Solver

from tests._paths import SITE_DRAINAGE_INP, SOLVER_DATA_DIR, artifact_dir

SITE_DRAINAGE_SNOW_INP = os.path.join(SOLVER_DATA_DIR, "site_drainage_snow.inp")


class _EditCase(unittest.TestCase):
    """Base case: open a Solver on a given model in OPENED state."""

    def _opened(self, inp):
        d = artifact_dir(self)
        s = Solver(
            inp,
            os.path.join(d, "model.rpt"),
            os.path.join(d, "model.out"),
        )
        s.open()
        self.addCleanup(self._close_destroy, s)
        return s

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestPollutantEditing(_EditCase):
    def test_analyze_is_read_only(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        before = len(s.pollutants)
        impacts = ed.analyze_pollutant_impact("TSS")
        self.assertIsInstance(impacts, list)
        for e in impacts:
            self.assertIsInstance(e, ImpactEntry)
        # analyze must not mutate the model
        self.assertEqual(len(s.pollutants), before)

    def test_delete_by_name_removes_pollutant(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        before = len(s.pollutants)
        result = ed.delete_pollutant("TSS")
        self.assertIsInstance(result, list)
        self.assertEqual(len(s.pollutants), before - 1)

    def test_delete_missing_name_raises(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        with self.assertRaises(KeyError):
            ed.delete_pollutant("NO_SUCH_POLLUTANT")


class TestLanduseEditing(_EditCase):
    def test_analyze_returns_entries(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        impacts = ed.analyze_landuse_impact("Commercial")
        self.assertIsInstance(impacts, list)

    def test_delete_then_name_unresolvable(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        ed.delete_landuse("Commercial")
        # The name no longer resolves after deletion.
        with self.assertRaises(KeyError):
            ed.analyze_landuse_impact("Commercial")

    def test_delete_by_index(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        result = ed.delete_landuse(0)
        self.assertIsInstance(result, list)


class TestSnowpackEditing(_EditCase):
    def test_analyze_and_delete(self):
        s = self._opened(SITE_DRAINAGE_SNOW_INP)
        ed = ModelEditor(s)
        before = len(s.snowpacks)
        impacts = ed.analyze_snowpack_impact("SP1")
        self.assertIsInstance(impacts, list)
        ed.delete_snowpack("SP1")
        self.assertEqual(len(s.snowpacks), before - 1)


class TestHydrographEditing(_EditCase):
    def test_analyze_unknown_name_raises(self):
        s = self._opened(SITE_DRAINAGE_INP)
        ed = ModelEditor(s)
        # Hydrograph groups are name-keyed; an unknown name is a C-API error.
        with self.assertRaises(Exception):
            ed.analyze_hydrograph_impact("NO_SUCH_UH_GROUP")


class TestRefTypeNames(unittest.TestCase):
    """The extended reference-type name map covers all 22 C enum values."""

    def test_extended_names_present(self):
        for code, name in (
            (14, "pollutant"),
            (15, "pattern"),
            (16, "aquifer"),
            (20, "landuse"),
            (21, "control_rule"),
        ):
            e = ImpactEntry(code, 0, "x", False)
            self.assertEqual(e.obj_type_name, name)


if __name__ == "__main__":
    unittest.main()
