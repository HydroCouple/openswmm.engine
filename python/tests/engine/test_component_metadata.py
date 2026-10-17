"""Configured model inspection uses the same public API without starting a run."""
from pathlib import Path
import shutil
import unittest

from openswmm.engine import Solver, EngineError, EngineState
from tests._paths import artifact_dir


class TestComponentMetadata(unittest.TestCase):
    def setUp(self):
        self.directory = Path(artifact_dir(self))
        self.model = self.directory / "model.inp"
        source = Path(__file__).resolve().parents[3] / "tests/unit/engine/data/component_metadata/reference.inp"
        shutil.copyfile(source, self.model)
        self.report = self.directory / "model.rpt"
        self.output = self.directory / "model.out"
        for path in (self.report, self.output):
            path.unlink(missing_ok=True)

    def test_inspection_exposes_model_and_requires_normal_reopen(self):
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open_metadata()
            self.assertEqual(solver.state, EngineState.OPENED)
            self.assertEqual([node.id for node in solver.nodes], ["J", "O"])
            self.assertEqual([pollutant.id for pollutant in solver.pollutants], ["A", "B"])
            self.assertFalse(self.report.exists())
            self.assertFalse(self.output.exists())
            with self.assertRaises(EngineError):
                solver.initialize()
            solver.close()
            solver.open()
            solver.initialize()
            solver.start()
            for _ in solver.steps():
                pass
            solver.end()
            solver.close()
            self.assertTrue(self.output.exists())
        finally:
            solver.destroy()

    def test_coordinate_presence_and_mesh_scale_before_simulation(self):
        with self.model.open("a") as model:
            model.write("\n[COORDINATES]\nJ 0 0\n[2D_OPTIONS]\nREPORT_2D NO\n"
                        "[2D_VERTICES]\n0 0 10\n10 0 10\n10 10 10\n"
                        "[2D_TRIANGLES]\n0 1 2 .03 0\n")
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open_metadata()
            self.assertTrue(solver.spatial.node_has_coord("J"))
            self.assertFalse(solver.spatial.node_has_coord("O"))
            self.assertEqual(solver.spatial.node_coord("J"), (0, 0))
            self.assertEqual(solver.surface2d.geometry_scale_to_si, .3048)
            self.assertEqual(solver.surface2d.n_vertices, 3)
            self.assertFalse(self.output.exists())
        finally:
            solver.destroy()

    def test_missing_runtime_plugin_is_not_loaded_during_inspection(self):
        with self.model.open("a") as model:
            model.write("\n[PLUGINS]\nINPUT missing-component-plugin.dylib\n")
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open_metadata()
            self.assertEqual(len(solver.nodes), 2)
            self.assertFalse(self.report.exists())
            self.assertFalse(self.output.exists())
        finally:
            solver.destroy()

    def test_reaction_registry_is_available_before_simulation(self):
        (self.directory / "model.rxn").write_text(
            "[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n"
            "[REACTION_PIPES]\nFORMULA X 2*A\n"
            "[REACTION_TANKS]\nFORMULA X 2*A\n"
        )
        with self.model.open("a") as model:
            model.write('\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config="model.rxn"\n')
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open_metadata()
            self.assertEqual([species.name for species in solver.reactions.species], ["X", "W"])
            self.assertFalse(self.output.exists())
        finally:
            solver.destroy()

    def test_outfall_receipt_is_a_named_snapshot_before_report_start(self):
        solver = Solver(self.model, self.report, self.output)
        try:
            solver.open()
            with self.assertRaises(EngineError):
                _ = solver.nodes["O"].outfall.export_receipt
            solver.initialize()
            solver.start()
            for _ in range(6):
                solver.step()
            first = solver.nodes["O"].outfall.export_receipt
            self.assertEqual(first["flags"], 7)
            self.assertGreater(first["exported_m3"], 0)
            self.assertGreater(first["exported_heat_j"], 0)
            self.assertEqual(first["reversed_m3"], 0)
            self.assertEqual(first["pollutant_amount_units"], {"A": "kg", "B": "kg"})
            self.assertGreater(first["pollutant_amounts"]["A"], 0)
            self.assertAlmostEqual(first["pollutant_amounts"]["A"], first["pollutant_amounts"]["B"], places=12)
            saved = first["exported_m3"]
            for _ in range(6):
                solver.step()
            second = solver.nodes["O"].outfall.export_receipt
            self.assertGreater(second["exported_m3"], first["exported_m3"])
            self.assertEqual(first["exported_m3"], saved)
            # Mutating the copied mapping cannot debit or alter native state.
            second["pollutant_amounts"]["A"] = -1
            self.assertGreater(solver.nodes["O"].outfall.export_receipt["pollutant_amounts"]["A"], 0)
        finally:
            solver.destroy()
