"""Hot-start inspection: contents, per-object records, consistent edits, legacy files."""
from datetime import datetime, timedelta, timezone
import math
from pathlib import Path
import shutil
import unittest

from openswmm.engine import BadHandleError, BadParamError, HotStart, HotStartError, Solver
from openswmm.engine._hotstart import HotStartNodeRecord

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

# Junctions J1, J2, storage ST1 (1000 ft2 functional), outfall O1, conduits C1-C3, TSS.
# The run saves a legacy SWMM 5 file at its end.
_DECK = """[TITLE]
hot-start inspection

[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2024
START_TIME 00:00:00
REPORT_START_DATE 01/01/2024
REPORT_START_TIME 00:00:00
END_DATE 01/01/2024
END_TIME 01:00:00
REPORT_STEP 00:05:00
WET_STEP 00:05:00
DRY_STEP 00:05:00
ROUTING_STEP 0:00:10

[FILES]
SAVE HOTSTART "{legacy}" LEGACY

[JUNCTIONS]
J1 100 10 2 0 0
J2 99 10 0 0 0

[STORAGE]
ST1 98 12 2 FUNCTIONAL 1000 0 0 0 0

[OUTFALLS]
O1 95 FREE NO

[CONDUITS]
C1 J1 J2 400 0.013 0 0 1.5 0
C2 J2 ST1 400 0.013 0 0 0 0
C3 ST1 O1 400 0.013 0 0 0 0

[XSECTIONS]
C1 CIRCULAR 2 0 0 0 1
C2 CIRCULAR 2 0 0 0 1
C3 CIRCULAR 2 0 0 0 1

[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 5

[COORDINATES]
J1 0 0
J2 400 0
ST1 800 0
O1 1200 0
"""


def _close(s: Solver) -> None:
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


def _near(a: float, b: float) -> bool:
    """Legacy files store single-precision values."""
    return math.isclose(a, b, rel_tol=1e-6)


class _InspectionCase(unittest.TestCase):
    """Runs the deck once per test: a legacy file from SAVE HOTSTART, a native one after end()."""

    def setUp(self):
        self.dir = Path(artifact_dir(self))
        self.inp = self.dir / "inspection.inp"
        self.legacy = self.dir / "end.hsf"
        self.native = self.dir / "end.hs"
        for p in (self.legacy, self.native):
            p.unlink(missing_ok=True)
        self.inp.write_text(_DECK.format(legacy=self.legacy.as_posix()))
        s = Solver(str(self.inp), str(self.dir / "inspection.rpt"),
                   str(self.dir / "inspection.out"))
        try:
            s.open()
            s.initialize()
            s.start()
            for _ in s.steps():
                pass
            s.end()
            HotStart.save_from(s, self.native)
            self.nodes = {n.id: (n.depth, n.head, n.volume, n.quality("TSS"))
                          for n in (s.nodes[i] for i in range(len(s.nodes)))}
            self.links = {k.id: (k.flow, k.depth) for k in (s.links[i] for i in range(len(s.links)))}
            s.report()
        finally:
            _close(s)
        self.assertTrue(self.legacy.exists())

    def opened(self, inp=None, name="opened") -> Solver:
        s = Solver(str(inp or self.inp), str(self.dir / f"{name}.rpt"), str(self.dir / f"{name}.out"))
        s.open()
        self.addCleanup(_close, s)
        return s


class TestNativeFile(_InspectionCase):
    def test_metadata(self):
        with HotStart.open(self.native) as hs:
            self.assertEqual(hs.contents, ("SPECIES",))
            self.assertGreater(hs.version, 0)
            self.assertEqual(hs.saved_at.tzinfo, timezone.utc)
            self.assertLess(abs(datetime.now(timezone.utc) - hs.saved_at), timedelta(hours=1))
            self.assertEqual(hs.end_datetime, datetime(2024, 1, 1, 1, 0))
            self.assertEqual(hs.species, ["TSS"])
            self.assertEqual(hs.subcatchment_count, 0)
            self.assertEqual(hs.subcatchment_records(), [])

    def test_records_hold_the_saved_state(self):
        with HotStart.open(self.native) as hs:
            nodes = hs.node_records()
            links = hs.link_records()
        self.assertTrue(all(isinstance(r, HotStartNodeRecord) for r in nodes))
        self.assertEqual({r.id for r in nodes}, set(self.nodes))
        for r in nodes:
            depth, head, volume, tss = self.nodes[r.id]
            self.assertAlmostEqual(r.depth, depth, places=9, msg=r.id)
            self.assertAlmostEqual(r.head, head, places=9, msg=r.id)
            self.assertAlmostEqual(r.volume, volume, places=6, msg=r.id)
            self.assertLess(r.age, 0.0, r.id)  # water age not tracked
            self.assertEqual(len(r.concentrations), 1)
            self.assertAlmostEqual(r.concentrations[0], tss, places=9, msg=r.id)
        self.assertGreater(self.nodes["ST1"][0], 0.0)  # the comparison is not all zeros
        self.assertEqual({r.id for r in links}, set(self.links))
        for r in links:
            flow, depth = self.links[r.id]
            self.assertAlmostEqual(r.flow, flow, places=9, msg=r.id)
            self.assertAlmostEqual(r.depth, depth, places=9, msg=r.id)
            self.assertFalse(math.isnan(r.volume), r.id)

    def test_state_edits_are_saved_and_derive_head_and_volume_at_restart(self):
        edited = self.dir / "edited.hs"
        shutil.copyfile(self.native, edited)
        with HotStart.open(edited) as hs:
            hs.set_node_state("ST1", 1.0)
            hs.set_link_state("C1", 0.5, 0.4)
        with HotStart.open(edited) as hs:  # the edits were written on close
            st1 = next(r for r in hs.node_records() if r.id == "ST1")
            c1 = next(r for r in hs.link_records() if r.id == "C1")
        self.assertEqual(st1.depth, 1.0)
        self.assertTrue(math.isnan(st1.head) and math.isnan(st1.volume))
        self.assertEqual((c1.flow, c1.depth), (0.5, 0.4))
        self.assertTrue(math.isnan(c1.volume))

        s = self.opened()
        s.initialize()
        with HotStart.open(edited) as hs:
            hs.apply(s)
        node = s.nodes["ST1"]
        self.assertAlmostEqual(node.depth, 1.0)
        self.assertAlmostEqual(node.head, node.invert_elev + 1.0)
        self.assertAlmostEqual(node.volume, 1000.0)  # 1000 ft2 x 1 ft
        self.assertAlmostEqual(s.links["C1"].flow, 0.5)
        self.assertAlmostEqual(s.links["C1"].depth, 0.4)

    def test_bad_state_edits_are_refused(self):
        with HotStart.open(self.native) as hs:
            with self.assertRaises(BadParamError):
                hs.set_node_state("J1", -1.0)
            with self.assertRaises(BadParamError):
                hs.set_node_state("nope", 1.0)
            with self.assertRaises(BadParamError):
                hs.set_link_state("C1", math.inf, 1.0)

    def test_closed_handle_raises(self):
        hs = HotStart.open(self.native)
        hs.close()
        with self.assertRaises(BadHandleError):
            hs.node_records()
        with self.assertRaises(BadHandleError):
            hs.species
        with self.assertRaises(BadHandleError):
            hs.contents


class TestLegacyFile(_InspectionCase):
    def test_reads_against_the_model(self):
        with HotStart.open(self.native) as hs:
            native_nodes = hs.node_records()
            native_links = hs.link_records()
        with HotStart.open_legacy(self.opened(), self.legacy) as hs:
            self.assertEqual(set(hs.contents), {"LEGACY", "SPECIES"})
            self.assertIn(hs.version, (1, 2, 3, 4))
            self.assertIsNone(hs.saved_at)
            self.assertIsNone(hs.end_datetime)
            self.assertEqual(hs.species, ["TSS"])
            nodes = hs.node_records()
            links = hs.link_records()
        self.assertEqual([r.id for r in nodes], [r.id for r in native_nodes])
        for r, n in zip(nodes, native_nodes):
            self.assertTrue(_near(r.depth, n.depth), r.id)
            self.assertTrue(math.isnan(r.head) and math.isnan(r.volume), r.id)
            self.assertTrue(_near(r.concentrations[0], n.concentrations[0]), r.id)
        self.assertEqual([r.id for r in links], [r.id for r in native_links])
        for r, n in zip(links, native_links):
            self.assertTrue(_near(r.flow, n.flow) and _near(r.depth, n.depth), r.id)
            self.assertTrue(math.isnan(r.volume), r.id)

    def test_handle_is_read_only(self):
        before = self.legacy.read_bytes()
        with HotStart.open_legacy(self.opened(), self.legacy) as hs:
            with self.assertRaises(HotStartError):
                hs.set_node_state("J1", 1.0)
            with self.assertRaises(HotStartError):
                hs.set_link_state("C1", 1.0, 1.0)
        self.assertEqual(self.legacy.read_bytes(), before)

    def test_another_model_or_a_native_file_is_refused(self):
        with self.assertRaises(HotStartError):
            HotStart.open_legacy(self.opened(SITE_DRAINAGE_INP, "other"), self.legacy)
        with self.assertRaises(HotStartError):
            HotStart.open_legacy(self.opened(), self.native)


if __name__ == "__main__":
    unittest.main()
