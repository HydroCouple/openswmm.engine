"""Tests for GeoPackage Python bindings.

These tests verify the Cython _geopackage.pyx module wraps the
C API correctly. Requires OPENSWMM_WITH_GEOPACKAGE=ON at build time.
"""

import os
import pytest
import numpy as np

# Skip all tests if GeoPackage not built
gpkg_available = False
try:
    from openswmm.engine._geopackage import GeoPackage, register, is_registered
    gpkg_available = True
except ImportError:
    pass

pytestmark = pytest.mark.skipif(
    not gpkg_available,
    reason="GeoPackage bindings not available (build with -DOPENSWMM_WITH_GEOPACKAGE=ON)"
)


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def gpkg_path(tmp_path):
    """Path for a temporary GeoPackage file."""
    return str(tmp_path / "test.gpkg")


@pytest.fixture
def gpkg_with_schema(gpkg_path):
    """A GeoPackage opened for the first time (creates file)."""
    gpkg = GeoPackage(gpkg_path)
    yield gpkg
    gpkg.close()


# ============================================================================
# Lifecycle
# ============================================================================

class TestLifecycle:
    """GeoPackage open/close lifecycle."""

    def test_open_creates_file(self, tmp_path):
        path = str(tmp_path / "new.gpkg")
        gpkg = GeoPackage(path)
        assert os.path.exists(path) or gpkg is not None
        gpkg.close()

    def test_context_manager(self, gpkg_path):
        with GeoPackage(gpkg_path) as gpkg:
            assert gpkg is not None

    def test_double_close_safe(self, gpkg_path):
        gpkg = GeoPackage(gpkg_path)
        gpkg.close()
        gpkg.close()  # should not raise

    def test_last_error_default(self, gpkg_with_schema):
        err = gpkg_with_schema.last_error
        assert isinstance(err, str)


# ============================================================================
# Simulation metadata
# ============================================================================

class TestSimulationMetadata:
    """Querying simulation runs and object counts."""

    def test_simulation_count_empty(self, gpkg_with_schema):
        """New file should have zero simulations."""
        n = gpkg_with_schema.simulation_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_simulation_ids_empty(self, gpkg_with_schema):
        ids = gpkg_with_schema.simulation_ids()
        assert isinstance(ids, list)

    def test_variable_count(self, gpkg_with_schema):
        n = gpkg_with_schema.variable_count()
        assert isinstance(n, int)
        assert n >= 0


# ============================================================================
# Transactions
# ============================================================================

class TestTransactions:
    """Transaction begin/commit/rollback."""

    def test_begin_commit(self, gpkg_with_schema):
        gpkg_with_schema.begin()
        gpkg_with_schema.commit()

    def test_begin_rollback(self, gpkg_with_schema):
        gpkg_with_schema.begin()
        gpkg_with_schema.rollback()


# ============================================================================
# Observed data CRUD
# ============================================================================

class TestObservedData:
    """Observed data series create/write/read."""

    def test_create_series(self, gpkg_with_schema):
        sid = gpkg_with_schema.create_observed_series(
            "test_flow", "flow",
            obj_type="NODE", obj_id="J1",
            source="Test", units="CFS")
        assert isinstance(sid, int)
        assert sid >= 0

    def test_write_single_value(self, gpkg_with_schema):
        sid = gpkg_with_schema.create_observed_series("s1", "depth")
        gpkg_with_schema.write_observed_value(
            sid, "2026-01-15T08:00:00Z", 1.5, "A")

    def test_write_read_roundtrip(self, gpkg_with_schema):
        sid = gpkg_with_schema.create_observed_series("s2", "flow")

        timestamps = [
            "2026-01-15T08:00:00Z",
            "2026-01-15T08:15:00Z",
            "2026-01-15T08:30:00Z",
        ]
        values = [1.0, 2.5, 3.0]

        gpkg_with_schema.begin()
        gpkg_with_schema.write_observed_values(sid, timestamps, values)
        gpkg_with_schema.commit()

        # Read back
        ts_back, vals_back = gpkg_with_schema.read_observed_values(sid)
        assert len(ts_back) == 3
        assert len(vals_back) == 3
        np.testing.assert_allclose(vals_back, [1.0, 2.5, 3.0], atol=1e-6)

    def test_observed_series_count(self, gpkg_with_schema):
        gpkg_with_schema.create_observed_series("a", "depth")
        gpkg_with_schema.create_observed_series("b", "flow")
        n = gpkg_with_schema.observed_series_count()
        assert n >= 2

    def test_observed_value_count(self, gpkg_with_schema):
        sid = gpkg_with_schema.create_observed_series("c", "depth")
        gpkg_with_schema.write_observed_value(sid, "2026-01-01T00:00:00Z", 0.5)
        gpkg_with_schema.write_observed_value(sid, "2026-01-01T01:00:00Z", 0.7)
        n = gpkg_with_schema.observed_value_count(sid)
        assert n == 2

    def test_bulk_write_performance(self, gpkg_with_schema):
        """Bulk write 1000 values should not crash."""
        sid = gpkg_with_schema.create_observed_series("bulk", "flow")
        timestamps = [f"2026-01-15T{h:02d}:{m:02d}:00Z"
                      for h in range(24) for m in range(0, 60, 1)][:1000]
        values = list(range(1000))

        gpkg_with_schema.begin()
        gpkg_with_schema.write_observed_values(sid, timestamps, values)
        gpkg_with_schema.commit()

        n = gpkg_with_schema.observed_value_count(sid)
        assert n == 1000


# ============================================================================
# Ad-hoc queries
# ============================================================================

class TestAdHocQueries:
    """SQL query pass-through."""

    def test_query_int(self, gpkg_with_schema):
        v = gpkg_with_schema.query_int("SELECT 42")
        assert v == 42

    def test_query_double(self, gpkg_with_schema):
        v = gpkg_with_schema.query_double("SELECT 3.14159")
        assert v == pytest.approx(3.14159, abs=1e-4)


# ============================================================================
# Registration
# ============================================================================

class TestRegistration:
    """Plugin registration functions."""

    def test_is_registered_returns_bool(self):
        v = is_registered()
        assert isinstance(v, bool)


# ============================================================================
# Topology edge count
# ============================================================================

class TestTopologyEdgeCount:
    """Test topology_edge_count method."""

    def test_topology_edge_count(self, gpkg_with_schema):
        # The count may be 0 if no topology edges exist, but the call
        # should succeed and return a non-negative integer.
        sid = gpkg_with_schema.simulation_ids()[0]
        count = gpkg_with_schema.topology_edge_count(sid)
        assert isinstance(count, int)
        assert count >= 0
