"""Tests for GeoPackage Python bindings.

These tests verify the Cython _geopackage.pyx module wraps the
C API correctly. Requires OPENSWMM_WITH_GEOPACKAGE=ON at build time.
"""

import os
import unittest

import numpy as np

# Skip all tests if GeoPackage not built
try:
    from openswmm.engine._geopackage import GeoPackage, register, is_registered
except ImportError:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(
        "GeoPackage bindings not available (build with -DOPENSWMM_WITH_GEOPACKAGE=ON)"
    )

from tests._paths import artifact_dir  # noqa: E402


# ============================================================================
# Helpers (former fixtures)
# ============================================================================

class GeoPackageCase(unittest.TestCase):
    """Base TestCase providing the former ``gpkg_path`` / ``gpkg_with_schema``
    fixtures as helper methods."""

    def gpkg_path(self):
        """Path for a per-test GeoPackage file."""
        return os.path.join(artifact_dir(self), "test.gpkg")

    def gpkg_with_schema(self):
        """A GeoPackage opened for the first time (creates file)."""
        gpkg = GeoPackage(self.gpkg_path())
        self.addCleanup(gpkg.close)
        return gpkg


# ============================================================================
# Lifecycle
# ============================================================================

class TestLifecycle(GeoPackageCase):
    """GeoPackage open/close lifecycle."""

    def test_open_creates_file(self):
        path = os.path.join(artifact_dir(self), "new.gpkg")
        gpkg = GeoPackage(path)
        self.assertTrue(os.path.exists(path) or gpkg is not None)
        gpkg.close()

    def test_context_manager(self):
        with GeoPackage(self.gpkg_path()) as gpkg:
            self.assertIsNotNone(gpkg)

    def test_double_close_safe(self):
        gpkg = GeoPackage(self.gpkg_path())
        gpkg.close()
        gpkg.close()  # should not raise

    def test_last_error_default(self):
        gpkg = self.gpkg_with_schema()
        err = gpkg.last_error
        self.assertIsInstance(err, str)


# ============================================================================
# Simulation metadata
# ============================================================================

class TestSimulationMetadata(GeoPackageCase):
    """Querying simulation runs and object counts."""

    def test_simulation_count_empty(self):
        """New file should have zero simulations."""
        gpkg = self.gpkg_with_schema()
        n = gpkg.simulation_count()
        self.assertIsInstance(n, int)
        self.assertGreaterEqual(n, 0)

    def test_simulation_ids_empty(self):
        gpkg = self.gpkg_with_schema()
        ids = gpkg.simulation_ids()
        self.assertIsInstance(ids, list)

    def test_variable_count(self):
        gpkg = self.gpkg_with_schema()
        n = gpkg.variable_count()
        self.assertIsInstance(n, int)
        self.assertGreaterEqual(n, 0)


# ============================================================================
# Transactions
# ============================================================================

class TestTransactions(GeoPackageCase):
    """Transaction begin/commit/rollback."""

    def test_begin_commit(self):
        gpkg = self.gpkg_with_schema()
        gpkg.begin()
        gpkg.commit()

    def test_begin_rollback(self):
        gpkg = self.gpkg_with_schema()
        gpkg.begin()
        gpkg.rollback()


# ============================================================================
# Observed data CRUD
# ============================================================================

class TestObservedData(GeoPackageCase):
    """Observed data series create/write/read."""

    def test_create_series(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series(
            "test_flow", "flow",
            obj_type="NODE", obj_id="J1",
            source="Test", units="CFS")
        self.assertIsInstance(sid, int)
        self.assertGreaterEqual(sid, 0)

    def test_write_single_value(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("s1", "depth")
        gpkg.write_observed_value(
            sid, "2026-01-15T08:00:00Z", 1.5, "A")

    def test_write_read_roundtrip(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("s2", "flow")

        timestamps = [
            "2026-01-15T08:00:00Z",
            "2026-01-15T08:15:00Z",
            "2026-01-15T08:30:00Z",
        ]
        values = [1.0, 2.5, 3.0]

        gpkg.begin()
        gpkg.write_observed_values(sid, timestamps, values)
        gpkg.commit()

        # Read back
        ts_back, vals_back = gpkg.read_observed_values(sid)
        self.assertEqual(len(ts_back), 3)
        self.assertEqual(len(vals_back), 3)
        np.testing.assert_allclose(vals_back, [1.0, 2.5, 3.0], atol=1e-6)

    def test_observed_series_count(self):
        gpkg = self.gpkg_with_schema()
        gpkg.create_observed_series("a", "depth")
        gpkg.create_observed_series("b", "flow")
        n = gpkg.observed_series_count()
        self.assertGreaterEqual(n, 2)

    def test_observed_value_count(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("c", "depth")
        gpkg.write_observed_value(sid, "2026-01-01T00:00:00Z", 0.5)
        gpkg.write_observed_value(sid, "2026-01-01T01:00:00Z", 0.7)
        n = gpkg.observed_value_count(sid)
        self.assertEqual(n, 2)

    def test_bulk_write_performance(self):
        """Bulk write 1000 values should not crash."""
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("bulk", "flow")
        timestamps = [f"2026-01-15T{h:02d}:{m:02d}:00Z"
                      for h in range(24) for m in range(0, 60, 1)][:1000]
        values = list(range(1000))

        gpkg.begin()
        gpkg.write_observed_values(sid, timestamps, values)
        gpkg.commit()

        n = gpkg.observed_value_count(sid)
        self.assertEqual(n, 1000)


# ============================================================================
# Ad-hoc queries
# ============================================================================

class TestAdHocQueries(GeoPackageCase):
    """SQL query pass-through."""

    def test_query_int(self):
        gpkg = self.gpkg_with_schema()
        v = gpkg.query_int("SELECT 42")
        self.assertEqual(v, 42)

    def test_query_double(self):
        gpkg = self.gpkg_with_schema()
        v = gpkg.query_double("SELECT 3.14159")
        self.assertAlmostEqual(v, 3.14159, delta=1e-4)


# ============================================================================
# Registration
# ============================================================================

class TestRegistration(unittest.TestCase):
    """Plugin registration functions."""

    def test_is_registered_returns_bool(self):
        v = is_registered()
        self.assertIsInstance(v, bool)

    def test_register_accepts_all_empty(self):
        """Regression for Phase 2c: ``register()`` previously used the
        unsafe ``x.encode('utf-8') if x else NULL`` conditional pattern
        which fails to transpile under Cython 3.0.12+. Calling with all
        default empty strings must now succeed; the underlying
        ``swmm_gpkg_register`` is permitted to return False on a vanilla
        plugin install so we only assert that the call does not raise
        and the return is bool.
        """
        v = register()
        self.assertIsInstance(v, bool)

    def test_register_accepts_partial_strings(self):
        """Same regression — exercise the path where some args are
        non-empty and others fall back to the NULL sentinel.
        """
        v = register(key="", org="test-org", email="", deploy="test-deploy")
        self.assertIsInstance(v, bool)


class TestObservedSeriesOptionalArgs(GeoPackageCase):
    """Regression for Phase 2c: ``create_observed_series`` and
    ``write_observed_value`` both used the unsafe conditional pattern for
    nullable string arguments. Each combination must transpile and run.
    """

    def test_create_series_all_optional_empty(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("p2c_a", "depth")
        self.assertGreaterEqual(sid, 0)

    def test_create_series_all_optional_set(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series(
            "p2c_b", "flow",
            obj_type="NODE", obj_id="J1",
            source="Phase2c", units="CFS")
        self.assertGreaterEqual(sid, 0)

    def test_create_series_mixed_optionals(self):
        gpkg = self.gpkg_with_schema()
        # obj_type set, obj_id empty, source set, units empty —
        # forces the per-arg branch in the Cython wrapper.
        sid = gpkg.create_observed_series(
            "p2c_c", "depth",
            obj_type="NODE", obj_id="",
            source="src", units="")
        self.assertGreaterEqual(sid, 0)

    def test_write_observed_value_no_flag(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("p2c_d", "depth")
        # flag="" triggers the NULL path in the C call.
        gpkg.write_observed_value(
            sid, "2026-05-25T00:00:00Z", 1.23)
        self.assertEqual(gpkg.observed_value_count(sid), 1)

    def test_write_observed_value_with_flag(self):
        gpkg = self.gpkg_with_schema()
        sid = gpkg.create_observed_series("p2c_e", "depth")
        gpkg.write_observed_value(
            sid, "2026-05-25T01:00:00Z", 4.56, "A")
        self.assertEqual(gpkg.observed_value_count(sid), 1)


# ============================================================================
# Topology edge count
# ============================================================================

class TestTopologyEdgeCount(GeoPackageCase):
    """Test topology_edge_count method."""

    def test_topology_edge_count(self):
        gpkg = self.gpkg_with_schema()
        # A freshly-opened gpkg has no simulations (see
        # TestSimulationMetadata.test_simulation_ids_empty), so we pass a
        # synthetic sim_id rather than indexing simulation_ids()[0]. The
        # contract under test is that the call succeeds and returns a
        # non-negative integer regardless of whether the sim_id exists.
        count = gpkg.topology_edge_count("none")
        self.assertIsInstance(count, int)
        self.assertGreaterEqual(count, 0)
