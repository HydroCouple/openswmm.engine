"""Recorded (undoable) deletes and the simulation-period open."""
from pathlib import Path
import shutil
import unittest

from openswmm.engine import (
    BadParamError,
    DeleteRecord,
    EngineError,
    EngineState,
    LinkType,
    ModelBuilder,
    ModelEditor,
    NodeType,
    Solver,
)
from tests._paths import artifact_dir


def _network() -> ModelBuilder:
    """A-B-C-D chain (three conduits), two subcatchments and two rain gages."""
    m = ModelBuilder()
    for name in ("A", "B", "C"):
        m.add_node(name, NodeType.JUNCTION)
    m.add_node("D", NodeType.OUTFALL)
    for name in ("AB", "BC", "CD"):
        m.add_link(name, LinkType.CONDUIT)
    m.set_link_nodes(0, 0, 1)
    m.set_link_nodes(1, 1, 2)
    m.set_link_nodes(2, 2, 3)
    m.add_subcatch("S1")
    m.add_subcatch("S2")
    m.add_gage("G1")
    m.add_gage("G2")
    return m


def _counts(ed: ModelEditor) -> tuple:
    return ed.node_count, ed.link_count, ed.subcatch_count, ed.gage_count


def _impacts(entries) -> list:
    return [(e.obj_type, e.obj_idx, e.field, e.cascaded) for e in entries]


class TestRecordedDelete(unittest.TestCase):
    def test_node_restore_brings_back_node_links_and_positions(self):
        m = _network()
        ed = ModelEditor(m)
        before = _counts(ed)
        impact_by_name = _impacts(ed.analyze_node_impact("B"))
        impact_by_index = _impacts(ed.analyze_node_impact(1))

        impacts, record = ed.delete_node_recorded("B")
        self.assertIsInstance(record, DeleteRecord)
        # Same entries as the analysis; the delete reports them in its own order.
        self.assertEqual(sorted(_impacts(impacts)), sorted(impact_by_name))
        self.assertEqual((ed.node_count, ed.link_count), (3, 1))  # AB and BC cascade
        self.assertTrue(record.restorable)

        record.restore()
        self.assertFalse(record.restorable)
        self.assertEqual(_counts(ed), before)
        # Same name at the same index, with the same links attached.
        self.assertEqual(_impacts(ed.analyze_node_impact("B")), impact_by_name)
        self.assertEqual(_impacts(ed.analyze_node_impact(1)), impact_by_index)

    def test_link_subcatch_and_gage_restore(self):
        m = _network()
        ed = ModelEditor(m)
        before = _counts(ed)
        for delete, key in ((ed.delete_link_recorded, "BC"),
                            (ed.delete_subcatch_recorded, "S1"),
                            (ed.delete_gage_recorded, "G2")):
            _, record = delete(key)
            self.assertNotEqual(_counts(ed), before)
            record.restore()
            self.assertEqual(_counts(ed), before)
        # Restored names resolve again.
        ed.analyze_link_impact("BC")
        ed.analyze_subcatch_impact("S1")
        ed.analyze_gage_impact("G2")

    def test_batches_restore_as_one(self):
        m = _network()
        ed = ModelEditor(m)
        before = _counts(ed)
        impact_a = _impacts(ed.analyze_node_impact("A"))
        _, nodes = ed.delete_nodes_recorded(["A", "C"])
        _, subcatchments = ed.delete_subcatchments_recorded(["S2", "S1"])
        _, gages = ed.delete_gages_recorded(["G1"])
        _, links = ed.delete_links_recorded([])  # every link went with A and C
        self.assertEqual(_counts(ed), (2, 0, 0, 1))
        # Undo in reverse order of the deletes.
        for record in (links, gages, subcatchments, nodes):
            record.restore()
        self.assertEqual(_counts(ed), before)
        self.assertEqual(_impacts(ed.analyze_node_impact("A")), impact_a)

    def test_empty_batch_returns_a_no_op_record(self):
        ed = ModelEditor(_network())
        before = _counts(ed)
        impacts, record = ed.delete_links_recorded([])
        self.assertEqual(impacts, [])
        record.restore()
        self.assertEqual(_counts(ed), before)

    def test_record_restores_at_most_once_and_close_is_final(self):
        ed = ModelEditor(_network())
        _, record = ed.delete_gage_recorded("G1")
        record.restore()
        with self.assertRaises(ValueError):
            record.restore()

        _, record = ed.delete_gage_recorded("G1")
        record.close()
        record.close()  # safe twice
        self.assertFalse(record.restorable)
        with self.assertRaises(ValueError):
            record.restore()
        self.assertEqual(ed.gage_count, 1)

    def test_restore_refuses_when_the_model_moved_on(self):
        m = _network()
        ed = ModelEditor(m)
        _, record = ed.delete_gage_recorded("G1")
        m.add_gage("G1")  # the name is taken again
        count = ed.gage_count
        with self.assertRaises(BadParamError):
            record.restore()
        self.assertEqual(ed.gage_count, count)  # nothing changed
        self.assertTrue(record.restorable)

    def test_failed_delete_returns_no_record_and_changes_nothing(self):
        ed = ModelEditor(_network())
        before = _counts(ed)
        with self.assertRaises(EngineError):
            ed.delete_nodes_recorded([0, 99])
        self.assertEqual(_counts(ed), before)

    def test_record_cannot_be_constructed_directly(self):
        with self.assertRaises(TypeError):
            DeleteRecord()


class TestOpenSimulationPeriod(unittest.TestCase):
    def setUp(self):
        self.directory = Path(artifact_dir(self))
        self.model = self.directory / "model.inp"
        source = (Path(__file__).resolve().parents[3]
                  / "tests/unit/engine/data/component_metadata/reference.inp")
        shutil.copyfile(source, self.model)
        with self.model.open("a") as model:
            model.write("\n[COORDINATES]\nJ 0 0\n[2D_OPTIONS]\nREPORT_2D NO\n"
                        "[2D_VERTICES]\n0 0 10\n10 0 10\n10 10 10\n"
                        "[2D_TRIANGLES]\n0 1 2 .03 0\n")
        self.report = self.directory / "model.rpt"
        self.output = self.directory / "model.out"
        for path in (self.report, self.output):
            path.unlink(missing_ok=True)

    def _period(self, open_name):
        solver = Solver(self.model, self.report, self.output)
        try:
            getattr(solver, open_name)()
            self.assertEqual(solver.state, EngineState.OPENED)
            return solver.options.start_datetime, solver.options.end_datetime
        finally:
            solver.destroy()

    def test_period_matches_metadata_open(self):
        self.assertEqual(self._period("open_simulation_period"), self._period("open_metadata"))

    def test_leaves_out_the_mesh_and_requires_a_normal_reopen(self):
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open_simulation_period()
            self.assertEqual([node.id for node in solver.nodes], ["J", "O"])
            with self.assertRaises(BadParamError):  # the handle has no 2D mesh
                solver.surface2d.n_vertices
            self.assertFalse(self.report.exists())
            self.assertFalse(self.output.exists())
            with self.assertRaises(EngineError):
                solver.initialize()
        finally:
            solver.destroy()


if __name__ == "__main__":
    unittest.main()
