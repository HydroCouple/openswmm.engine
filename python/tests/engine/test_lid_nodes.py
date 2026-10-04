"""Storage-node LID layers and ports through the compiled Python bindings."""
from pathlib import Path
import unittest

from openswmm.engine import Solver, LidType, LidNodeLayer, LidNodeLayerKind as K, LidLayerTreatment, EngineError
from tests._paths import artifact_dir

DECK = """[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2004
END_DATE 01/01/2004
END_TIME 00:01:00
ROUTING_STEP 00:00:01
REPORT_STEP 00:01:00
[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 0
[STORAGE]
S 0 2 0 FUNCTIONAL 0 0 100 0 0
[OUTFALLS]
O 0 FREE NO
[ORIFICES]
D S O BOTTOM 0 0.6 NO 0
[XSECTIONS]
D CIRCULAR .1 0 0 0
"""


class TestLidNodes(unittest.TestCase):
    def setUp(self):
        self.directory = Path(artifact_dir(self))
        self.path = self.directory / "lid_node.inp"
        self.path.write_text(DECK)
        self.solver = Solver(str(self.path), str(self.directory / "lid_node.rpt"), str(self.directory / "lid_node.out"))
        self.solver.open()
        self.solver.infrastructure.lids.add("Stack", LidType.NODE)
        self.layers = [LidNodeLayer(K.SURFACE, (6, .1))]
        self.layers += [LidNodeLayer(K.MEDIA, (1, .45, .2, .08, 2, 10, 3)) for _ in range(40)]
        self.layers += [LidNodeLayer(K.AGGREGATE, (6, .4, 100)), LidNodeLayer(K.BOTTOM, (0, 0))]
        self.solver.infrastructure.lids.set_layers("Stack", self.layers)

    def tearDown(self):
        self.solver.close()
        self.solver.destroy()

    def test_arbitrary_layers_and_atomic_rejection(self):
        self.assertEqual(self.solver.infrastructure.lids.get_layers("Stack"), self.layers)
        self.solver.infrastructure.lids.assign_node("S", "Stack", initial_saturation=25)
        self.assertEqual(self.solver.infrastructure.lids.node_assignment("S"), (0, 25.0))
        self.assertIsNone(self.solver.infrastructure.lids.node_assignment("O"))
        with self.assertRaises(EngineError):
            self.solver.infrastructure.lids.set_layers("Stack", [LidNodeLayer(K.MEDIA, (12, .1, .2, .08, 2, 10, 3))])
        self.assertEqual(self.solver.infrastructure.lids.get_layers("Stack"), self.layers)
        self.solver.infrastructure.lids.set_outlet_anchor("D", 42, position="BOTTOM")
        self.assertEqual(self.solver.infrastructure.lids.get_outlet_anchor("D"), (42, "BOTTOM"))
        with self.assertRaises(EngineError):
            self.solver.infrastructure.lids.set_layers("Stack", [LidNodeLayer(K.AGGREGATE, (24, .4, 100))])
        self.solver.infrastructure.lids.remove_node("S")
        self.assertIsNone(self.solver.infrastructure.lids.node_assignment("S"))
        self.assertIsNone(self.solver.infrastructure.lids.get_outlet_anchor("D"))

    def test_runtime_profile_and_lifecycle(self):
        self.solver.infrastructure.lids.assign_node("S", "Stack", initial_saturation=25)
        self.assertEqual(self.solver.infrastructure.lids.node_profile("S"), [])
        self.solver.initialize()
        self.assertEqual(len(self.solver.infrastructure.lids.node_profile("S")), 202)
        self.solver.start()
        self.solver.step()
        for row in self.solver.infrastructure.lids.node_profile("S"):
            self.assertGreater(row["top"], row["bottom"])
            self.assertGreaterEqual(row["moisture"], 0)
            self.assertLessEqual(row["moisture"], 1)
        with self.assertRaises(EngineError):
            self.solver.infrastructure.lids.set_layers("Stack", self.layers)
        self.solver.end()

    def test_treatment_roundtrip_and_atomic_validation(self):
        lids = self.solver.infrastructure.lids
        rules = [LidLayerTreatment(2, "TSS", 25, 1.5, "R = 0.2")]
        lids.set_layers("Stack", self.layers, treatments=rules)
        self.assertEqual(lids.get_treatments("Stack"), rules)
        lids.set_layers("Stack", self.layers)
        self.assertEqual(lids.get_treatments("Stack"), rules)
        with self.assertRaises(EngineError):
            lids.set_layers("Stack", self.layers, treatments=[LidLayerTreatment(2, "TSS", 101)])
        self.assertEqual(lids.get_treatments("Stack"), rules)
        lids.set_layers("Stack", self.layers, treatments=[])
        self.assertEqual(lids.get_treatments("Stack"), [])

    def test_layer_parameter_shape(self):
        with self.assertRaises(ValueError):
            LidNodeLayer(K.MEDIA, (1, .4))
        with self.assertRaises(ValueError):
            self.solver.infrastructure.lids.set_outlet_anchor("D", 1, position="MIDDLE")
