"""Edge cases found by the MCP method contract (2026-09-30).

* Bulk reads and writes of an empty collection return empty arrays instead of
  failing (the C bulk calls refuse a zero count).
* 2D solver reads before ``initialize()`` raise ``LifecycleError``.
* GeoPackage failures are typed: transaction misuse is a ``LifecycleError``,
  anything else a ``GeoPackageError``; both stay ``RuntimeError``.
* ``ModelBuilder`` writes the legacy default start date (2004-01-01).
* ``StorageView.seep_rate`` is the constant-rate form of storage exfiltration.
* ``HotStart.sim_datetime`` is the moment the state was saved.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import os
import unittest
from datetime import datetime

import numpy as np

try:
    from openswmm.engine import (
        GeoPackageError,
        HotStart,
        LifecycleError,
        ModelBuilder,
        Solver,
        catalog,
    )
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TWOD_EXAMPLE_INP = os.path.join(os.path.dirname(_REPO_ROOT), "examples", "2d_complete_example.inp")

_STORAGE_INP = """[OPTIONS]
FLOW_UNITS CFS
START_DATE 01/01/2026
END_DATE   01/01/2026
END_TIME   01:00:00

[STORAGE]
;;Name  Elev  MaxDepth  InitDepth  Shape       Curve/Params
SU1     0     10        1          FUNCTIONAL  1000  0  0

[OUTFALLS]
O1      -1    FREE

[CONDUITS]
C1      SU1   O1   100   0.013   0   0

[XSECTIONS]
C1      CIRCULAR   1   0   0   0
"""

_EMPTY_INP = """[TITLE]
no elements

[OPTIONS]
FLOW_UNITS CFS
START_DATE 01/01/2026
END_DATE   01/01/2026
END_TIME   01:00:00
"""


class TestEmptyCollections(unittest.TestCase):
    """Every bulk property of an empty collection is an empty array."""

    def setUp(self):
        d = artifact_dir(self)
        inp = os.path.join(d, "empty.inp")
        with open(inp, "w", encoding="utf-8") as fh:
            fh.write(_EMPTY_INP)
        self.solver = Solver(inp, os.path.join(d, "empty.rpt"), os.path.join(d, "empty.out"))
        self.solver.open()
        self.addCleanup(self.solver.close)

    def test_collection_bulk_reads(self):
        collections = sorted({t["collection"] for t in catalog.targets().values()
                              if "collection" in t and "." not in t["collection"]})
        checked = 0
        for name in collections:
            col = catalog.resolve(self.solver, name, None)
            if len(col) != 0:
                continue
            for m in catalog.members(name):
                required = [p for p in m.get("params", []) if p["required"]]
                if m["form"] not in ("property", "method") or required:
                    continue
                with self.subTest(member=f"{name}.{m['name']}"):
                    value = getattr(col, m["name"])
                    if m["form"] == "method":
                        value = value()
                    if isinstance(value, np.ndarray):
                        self.assertEqual(value.shape[0], 0)
                    checked += 1
        self.assertGreater(checked, 25)

    def test_bulk_writes_and_statistics(self):
        self.solver.nodes.depths = np.empty(0)
        self.solver.links.flows = np.empty(0)
        self.assertEqual(len(self.solver.statistics.node_max_depth), 0)
        self.assertEqual(self.solver.spatial.node_coords().shape, (0, 2))


@unittest.skipUnless(os.path.isfile(TWOD_EXAMPLE_INP), "2D example model not present")
class TestSurface2DLifecycle(unittest.TestCase):
    def test_solver_reads_before_initialize_are_lifecycle_errors(self):
        d = artifact_dir(self)
        s = Solver(TWOD_EXAMPLE_INP, os.path.join(d, "t.rpt"), os.path.join(d, "t.out"))
        s.open()
        self.addCleanup(s.close)
        with self.assertRaises(LifecycleError):
            s.surface2d.get_depths()
        s.initialize()
        self.assertEqual(len(s.surface2d.get_depths()), s.surface2d.n_triangles)


class TestGeoPackageErrors(unittest.TestCase):
    def setUp(self):
        try:
            from openswmm.engine import GeoPackage
        except ImportError:
            self.skipTest("engine built without GeoPackage")
        self.gpkg = GeoPackage(os.path.join(artifact_dir(self), "edge.gpkg"))
        self.addCleanup(self.gpkg.close)

    def test_transaction_misuse_is_a_lifecycle_error(self):
        with self.assertRaises(LifecycleError) as ctx:
            self.gpkg.commit()
        self.assertIsInstance(ctx.exception, RuntimeError)
        self.gpkg.begin()
        with self.assertRaises(LifecycleError):
            self.gpkg.begin()
        self.gpkg.rollback()

    def test_an_unopenable_file_is_a_geopackage_error(self):
        from openswmm.engine import GeoPackage

        missing = os.path.join(artifact_dir(self), "no_such_dir", "x.gpkg")
        with self.assertRaises(GeoPackageError) as ctx:
            GeoPackage(missing)
        self.assertIsInstance(ctx.exception, RuntimeError)


class TestModelBuilderDefaults(unittest.TestCase):
    def test_default_start_date_is_the_legacy_default(self):
        path = os.path.join(artifact_dir(self), "built.inp")
        b = ModelBuilder()
        self.assertEqual(b.add_node("J1", 0), 0)
        b.write(path)
        with open(path, encoding="utf-8") as fh:
            options = {t[0]: t[1] for t in (line.split() for line in fh) if len(t) >= 2}
        self.assertEqual(options["START_DATE"], "01/01/2004")


class TestStorageSeepRate(unittest.TestCase):
    """seep_rate is the constant-rate form of the storage exfiltration."""

    def test_seep_rate_is_constant_rate_exfiltration(self):
        d = artifact_dir(self)
        inp = os.path.join(d, "storage.inp")
        with open(inp, "w", encoding="utf-8") as fh:
            fh.write(_STORAGE_INP)
        s = Solver(inp, os.path.join(d, "s.rpt"), os.path.join(d, "s.out"))
        s.open()
        self.addCleanup(s.close)
        storage = s.nodes["SU1"]
        storage.storage.seep_rate = 0.5
        self.assertEqual(storage.storage.exfil_params, (0.0, 0.5, 0.0))
        self.assertEqual(storage.storage.seep_rate, 0.5)
        storage.storage.exfil_params = (4.0, 0.2, 0.3)  # Green-Ampt: no constant rate
        self.assertEqual(storage.storage.seep_rate, 0.0)


class TestHotStartMoment(unittest.TestCase):
    def test_sim_datetime_is_the_moment_saved(self):
        d = artifact_dir(self)
        path = os.path.join(d, "mid.hsf")
        with Solver(SITE_DRAINAGE_INP, os.path.join(d, "h.rpt"), os.path.join(d, "h.out")) as s:
            for _ in range(20):
                s.step()
            saved_at = s.current_datetime
            HotStart.save_from(s, path)
        with HotStart.open(path) as hs:
            self.assertIsInstance(hs.sim_datetime, datetime)
            self.assertLess(abs((hs.sim_datetime - saved_at).total_seconds()), 1.0)
            self.assertLess(hs.start_datetime, hs.sim_datetime)


if __name__ == "__main__":
    unittest.main()
