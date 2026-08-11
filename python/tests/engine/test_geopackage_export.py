"""Round-trip tests for Solver model -> GeoPackage export.

Exercises the Python export surface added so a loaded model can be written to a
GeoPackage with a usable CRS (without which features carry an undefined SRS and
GIS tools cannot place them):

  * ``Solver.write_geopackage(path, crs=...)`` — convenience wrapper.
  * ``Solver.write_with_plugin(path, GEOPACKAGE_PLUGIN_ID)`` — general form.

Verification reads the produced ``.gpkg`` back with stdlib ``sqlite3`` and
checks the GeoPackage magic, the registered SRS, that every feature layer is
tagged with that SRS, and that a node geometry blob carries it too.
"""

from __future__ import annotations

import os
import sqlite3
import struct
import unittest

from openswmm.engine import HAS_GEOPACKAGE, GEOPACKAGE_PLUGIN_ID, EngineError

from tests._paths import artifact_dir
from tests.engine._solver_cases import EngineSolverCase

_GPKG_MAGIC = 0x47504B47  # 'GPKG'


def _blob_srs_id(blob: bytes) -> int:
    """Read the srs_id field (bytes 4..7) of a GeoPackageBinary geometry blob.

    Per the OGC GeoPackage spec, flags-byte bit 0 is the header byte order:
    1 = little-endian, 0 = big-endian.
    """
    little_endian = (blob[3] & 0x01) == 1
    return struct.unpack_from("<i" if little_endian else ">i", blob, 4)[0]


@unittest.skipIf(
    not HAS_GEOPACKAGE,
    "GeoPackage not built (configure with -DOPENSWMM_WITH_GEOPACKAGE=ON)",
)
class TestGeoPackageExport(EngineSolverCase):
    def test_write_geopackage_with_crs(self):
        solver = self.opened_solver()
        gpkg = os.path.join(artifact_dir(self), "model.gpkg")
        n_nodes = len(solver.nodes)

        solver.write_geopackage(gpkg, crs="EPSG:2284")

        # CRS landed on the spatial frame (the field the writer reads).
        self.assertEqual(solver.spatial.crs, "EPSG:2284")

        con = sqlite3.connect(gpkg)
        try:
            cur = con.cursor()
            self.assertEqual(
                cur.execute("PRAGMA application_id").fetchone()[0], _GPKG_MAGIC)
            # SRS registered, resolvable by org+code.
            self.assertEqual(
                cur.execute(
                    "SELECT organization, organization_coordsys_id "
                    "FROM gpkg_spatial_ref_sys WHERE srs_id=2284"
                ).fetchone(),
                ("EPSG", 2284),
            )
            # Every feature layer tagged with the SRS.
            srs_ids = {r[0] for r in cur.execute(
                "SELECT srs_id FROM gpkg_geometry_columns")}
            self.assertEqual(srs_ids, {2284}, srs_ids)
            # Node count round-trips.
            self.assertEqual(
                cur.execute("SELECT COUNT(*) FROM nodes").fetchone()[0], n_nodes)
            # A node geometry blob carries the SRS too (when coordinates exist).
            row = cur.execute(
                "SELECT geom FROM nodes WHERE geom IS NOT NULL LIMIT 1").fetchone()
            if row and row[0]:
                self.assertEqual(_blob_srs_id(row[0]), 2284)
        finally:
            con.close()

    def test_write_with_plugin_explicit_id(self):
        solver = self.opened_solver()
        gpkg = os.path.join(artifact_dir(self), "model2.gpkg")
        solver.spatial.crs = "EPSG:4326"
        solver.write_with_plugin(gpkg, GEOPACKAGE_PLUGIN_ID)
        con = sqlite3.connect(gpkg)
        try:
            self.assertEqual(
                con.execute("SELECT COUNT(*) FROM nodes").fetchone()[0],
                len(solver.nodes),
            )
            self.assertEqual(
                con.execute(
                    "SELECT srs_id FROM gpkg_spatial_ref_sys WHERE srs_id=4326"
                ).fetchone(),
                (4326,),
            )
        finally:
            con.close()

    def test_unknown_plugin_id_raises(self):
        solver = self.opened_solver()
        with self.assertRaises(EngineError):
            solver.write_with_plugin(
                os.path.join(artifact_dir(self), "x.gpkg"),
                "org.not.a.real.plugin")

    def test_plugin_id_constant(self):
        self.assertEqual(
            GEOPACKAGE_PLUGIN_ID, "org.hydrocouple.openswmm.plugins.geopackage")
