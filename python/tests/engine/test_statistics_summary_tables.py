"""Report summary tables on :class:`Statistics` (``swmm_summary_*``).

The engine computes these tables with the code that writes the ``.rpt``; the
engine's own unit test compares them with the report text. These tests cover
the Python surface: every table reads, the per-cell reads agree with the
whole-table read, two columns agree with the independent statistics getters,
and the lifecycle and index errors are typed.
"""

from __future__ import annotations

import math
import unittest

import numpy as np

try:
    import openswmm.engine._statistics  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (
    BadIndexError,
    LifecycleError,
    ReportTable,
    SummaryObject,
    SummaryRow,
    SummaryTable,
)

from tests.engine._solver_cases import EngineSolverCase

TITLES = {
    SummaryTable.NODE_DEPTH: "Node Depth Summary",
    SummaryTable.NODE_INFLOW: "Node Inflow Summary",
    SummaryTable.NODE_SURCHARGE: "Node Surcharge Summary",
    SummaryTable.NODE_FLOODING: "Node Flooding Summary",
    SummaryTable.STORAGE_VOLUME: "Storage Volume Summary",
    SummaryTable.OUTFALL_LOADING: "Outfall Loading Summary",
    SummaryTable.LINK_FLOW: "Link Flow Summary",
    SummaryTable.FLOW_CLASSIFICATION: "Flow Classification Summary",
    SummaryTable.CONDUIT_SURCHARGE: "Conduit Surcharge Summary",
    SummaryTable.PUMPING: "Pumping Summary",
    SummaryTable.LID_PERFORMANCE: "LID Performance Summary",
}


def _column(table: ReportTable, name: str) -> int:
    return [c.name for c in table.columns].index(name)


class TestSummaryTables(EngineSolverCase):
    def test_every_table_reads_in_report_order(self):
        solver = self.completed_solver()
        tables = solver.statistics.summary_tables()
        self.assertEqual([t.table for t in tables], list(SummaryTable))
        counts = {
            SummaryObject.NODE: len(solver.nodes),
            SummaryObject.LINK: len(solver.links),
            SummaryObject.SUBCATCHMENT: len(solver.subcatchments),
        }
        for t in tables:
            with self.subTest(table=t.table.name):
                self.assertIsInstance(t, ReportTable)
                self.assertEqual(t.title, TITLES[t.table])
                self.assertGreater(len(t.columns), 0)
                self.assertEqual(t.values.dtype, np.float64)
                self.assertEqual(t.values.shape, (len(t.rows), len(t.columns)))
                for row in t.rows:
                    self.assertIsInstance(row.object_type, SummaryObject)
                    if row.object_type == SummaryObject.SYSTEM:
                        self.assertEqual(row.object_index, -1)
                    else:
                        self.assertGreaterEqual(row.object_index, 0)
                        self.assertLess(row.object_index, counts[row.object_type])

    def test_node_depth_lists_every_node_with_its_peak_depth(self):
        solver = self.completed_solver()
        table = solver.statistics.summary_table(SummaryTable.NODE_DEPTH)
        self.assertEqual([r.object_index for r in table.rows], list(range(len(solver.nodes))))
        self.assertTrue(all(r.object_type == SummaryObject.NODE for r in table.rows))
        self.assertTrue(all(r.label for r in table.rows))  # JUNCTION, OUTFALL, ...
        np.testing.assert_allclose(table.values[:, _column(table, "Maximum Depth")],
                                   solver.statistics.node_max_depth, rtol=1e-12)

    def test_link_flow_peak_matches_link_statistics(self):
        solver = self.completed_solver()
        table = solver.statistics.summary_table(SummaryTable.LINK_FLOW)
        self.assertEqual([r.object_index for r in table.rows], list(range(len(solver.links))))
        np.testing.assert_allclose(table.values[:, _column(table, "Maximum |Flow|")],
                                   np.abs(solver.statistics.link_max_flow), rtol=1e-12)

    def test_outfall_loading_ends_with_a_system_row(self):
        solver = self.completed_solver()
        table = solver.statistics.summary_table(SummaryTable.OUTFALL_LOADING)
        self.assertGreater(len(table.rows), 1)
        self.assertEqual(table.rows[-1], SummaryRow(SummaryObject.SYSTEM, -1, ""))

    def test_per_cell_reads_match_the_table(self):
        solver = self.completed_solver()
        stats = solver.statistics
        checked = 0
        for t in stats.summary_tables():
            for r, row in enumerate(t.rows):
                self.assertEqual(stats.summary_row_at(t.table, r), row)
                for c in range(len(t.columns)):
                    expected = t.values[r, c]
                    got = stats.summary_value_at(t.table, r, c)
                    if math.isnan(expected):
                        self.assertTrue(math.isnan(got), (t.title, r, c))
                    else:
                        self.assertEqual(got, expected, (t.title, r, c))
                    checked += 1
        self.assertGreater(checked, 0)

    def test_tables_are_refused_before_the_run_ends(self):
        solver = self.running_solver()
        with self.assertRaises(LifecycleError):
            solver.statistics.summary_table(SummaryTable.NODE_DEPTH)

    def test_unknown_tables_and_cells_are_refused(self):
        solver = self.completed_solver()
        stats = solver.statistics
        with self.assertRaises(BadIndexError):
            stats.summary_table(len(SummaryTable))
        rows = len(stats.summary_table(SummaryTable.NODE_DEPTH).rows)
        with self.assertRaises(BadIndexError):
            stats.summary_row_at(SummaryTable.NODE_DEPTH, rows)
        with self.assertRaises(BadIndexError):
            stats.summary_value_at(SummaryTable.NODE_DEPTH, 0, 99)


if __name__ == "__main__":
    unittest.main()
