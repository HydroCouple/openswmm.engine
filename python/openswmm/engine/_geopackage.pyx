"""
GeoPackage Access
=================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`GeoPackage` class provides read/write access to SWMM GeoPackage
databases for querying multi-run simulation results, importing observed data,
and performing spatial analysis.

.. code-block:: python

    from openswmm.engine import GeoPackage

    with GeoPackage("model.gpkg") as gpkg:
        sims = gpkg.simulation_ids()
        times, depths = gpkg.read_result_ts(sims[0], "NODE", "J1", "depth")
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

cdef extern from "openswmm_geopackage.h":
    ctypedef void* SWMM_Gpkg

    SWMM_Gpkg swmm_gpkg_open(const char* path)
    void swmm_gpkg_close(SWMM_Gpkg gpkg)
    const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg)

    int swmm_gpkg_begin(SWMM_Gpkg gpkg)
    int swmm_gpkg_commit(SWMM_Gpkg gpkg)
    int swmm_gpkg_rollback(SWMM_Gpkg gpkg)

    int swmm_gpkg_register(const char* key, const char* org,
                           const char* email, const char* deploy)
    int swmm_gpkg_is_registered()

    int swmm_gpkg_simulation_count(SWMM_Gpkg gpkg)
    int swmm_gpkg_simulation_id(SWMM_Gpkg gpkg, int index, char* buf, int bufsz)

    int swmm_gpkg_node_count(SWMM_Gpkg gpkg, const char* sim_id)
    int swmm_gpkg_link_count(SWMM_Gpkg gpkg, const char* sim_id)
    int swmm_gpkg_subcatch_count(SWMM_Gpkg gpkg, const char* sim_id)
    int swmm_gpkg_gage_count(SWMM_Gpkg gpkg, const char* sim_id)
    int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* sim_id)
    int swmm_gpkg_variable_count(SWMM_Gpkg gpkg)

    int swmm_gpkg_result_ts_count(SWMM_Gpkg gpkg, const char* sim_id,
                                   const char* obj_type, const char* obj_id,
                                   const char* var_name)
    int swmm_gpkg_read_result_ts(SWMM_Gpkg gpkg, const char* sim_id,
                                  const char* obj_type, const char* obj_id,
                                  const char* var_name,
                                  double* times, double* values, int max_count)

    int swmm_gpkg_read_summary(SWMM_Gpkg gpkg, const char* sim_id,
                                const char* obj_type, const char* obj_id,
                                const char* var_name, double* value)

    int swmm_gpkg_create_observed_series(SWMM_Gpkg gpkg, const char* name,
                                          const char* var_name, const char* obj_type,
                                          const char* obj_id, const char* source,
                                          const char* units)
    int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg, int series_id,
                                        const char* timestamp, double value,
                                        const char* quality_flag)
    int swmm_gpkg_write_observed_values(SWMM_Gpkg gpkg, int series_id,
                                         const char** timestamps,
                                         const double* values,
                                         const char** quality_flags, int count)

    int swmm_gpkg_observed_series_count(SWMM_Gpkg gpkg)
    int swmm_gpkg_observed_value_count(SWMM_Gpkg gpkg, int series_id)
    int swmm_gpkg_read_observed_values(SWMM_Gpkg gpkg, int series_id,
                                        char* timestamps, int ts_buf_len,
                                        double* values, int max_count)

    int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql)
    int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql, double* result)


cdef class GeoPackage:
    """GeoPackage database access for SWMM models, results, and observed data.

    :param path: Path to the .gpkg file.

    Supports context manager protocol::

        with GeoPackage("model.gpkg") as gpkg:
            print(gpkg.simulation_count())
    """

    cdef SWMM_Gpkg _handle

    def __cinit__(self, str path):
        cdef bytes b = path.encode('utf-8')
        self._handle = swmm_gpkg_open(b)
        if self._handle is NULL:
            raise RuntimeError(f"Failed to open GeoPackage: {path}")

    def __dealloc__(self):
        if self._handle is not NULL:
            swmm_gpkg_close(self._handle)
            self._handle = NULL

    def close(self):
        """Close the database connection."""
        if self._handle is not NULL:
            swmm_gpkg_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    @property
    def last_error(self) -> str:
        """Get the last error message."""
        cdef const char* msg = swmm_gpkg_last_error(self._handle)
        return msg.decode('utf-8') if msg else ""

    # ------------------------------------------------------------------
    # Transactions
    # ------------------------------------------------------------------

    def begin(self):
        """Begin a transaction for bulk operations."""
        if swmm_gpkg_begin(self._handle) != 0:
            raise RuntimeError(f"Transaction begin failed: {self.last_error}")

    def commit(self):
        """Commit the current transaction."""
        if swmm_gpkg_commit(self._handle) != 0:
            raise RuntimeError(f"Transaction commit failed: {self.last_error}")

    def rollback(self):
        """Roll back the current transaction."""
        if swmm_gpkg_rollback(self._handle) != 0:
            raise RuntimeError(f"Transaction rollback failed: {self.last_error}")

    # ------------------------------------------------------------------
    # Simulation metadata
    # ------------------------------------------------------------------

    def simulation_count(self) -> int:
        """Get the number of simulation runs in the file."""
        return swmm_gpkg_simulation_count(self._handle)

    def simulation_ids(self) -> list:
        """Get all simulation IDs as a list of strings."""
        cdef int n = swmm_gpkg_simulation_count(self._handle)
        cdef char buf[256]
        result = []
        for i in range(n):
            if swmm_gpkg_simulation_id(self._handle, i, buf, 256) == 0:
                result.append(buf.decode('utf-8'))
        return result

    def object_counts(self, str sim_id) -> dict:
        """Get model object counts for a simulation.

        :param sim_id: Simulation identifier.
        :returns: dict with keys: nodes, links, subcatchments, gages.
        """
        cdef bytes b = sim_id.encode('utf-8')
        return {
            "nodes": swmm_gpkg_node_count(self._handle, b),
            "links": swmm_gpkg_link_count(self._handle, b),
            "subcatchments": swmm_gpkg_subcatch_count(self._handle, b),
            "gages": swmm_gpkg_gage_count(self._handle, b),
        }

    def variable_count(self) -> int:
        """Get the number of output variables defined."""
        return swmm_gpkg_variable_count(self._handle)

    def topology_edge_count(self, str sim_id) -> int:
        """Get the number of topology edges for a simulation.

        :param sim_id: Simulation identifier.
        :returns: Topology edge count.
        :rtype: int
        """
        cdef bytes b = sim_id.encode('utf-8')
        return swmm_gpkg_topology_edge_count(self._handle, b)

    # ------------------------------------------------------------------
    # Result timeseries (zero-copy numpy)
    # ------------------------------------------------------------------

    def result_ts_count(self, str sim_id, str obj_type, str obj_id,
                        str variable) -> int:
        """Get the number of result timeseries records for a query.

        :param sim_id: Simulation identifier.
        :param obj_type: "NODE", "LINK", "SUBCATCH", or "SYSTEM".
        :param obj_id: Object identifier (e.g., "J1").
        :param variable: Variable name (e.g., "depth", "flow").
        :returns: Record count.
        """
        return swmm_gpkg_result_ts_count(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'))

    def read_result_ts(self, str sim_id, str obj_type, str obj_id,
                       str variable):
        """Read a result timeseries as numpy arrays.

        :param sim_id: Simulation identifier.
        :param obj_type: "NODE", "LINK", "SUBCATCH", or "SYSTEM".
        :param obj_id: Object identifier.
        :param variable: Variable name.
        :returns: Tuple of (times, values) as numpy float64 arrays.
        """
        cdef int n = swmm_gpkg_result_ts_count(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'))
        if n <= 0:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        cdef np.ndarray[double, ndim=1] times = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] values = np.empty(n, dtype=np.float64)
        cdef int read = swmm_gpkg_read_result_ts(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'),
            <double*>times.data, <double*>values.data, n)
        return times[:read], values[:read]

    # ------------------------------------------------------------------
    # Summary statistics
    # ------------------------------------------------------------------

    def read_summary(self, str sim_id, str obj_type, str obj_id,
                     str variable) -> float:
        """Read a single summary statistic value.

        :param sim_id: Simulation identifier.
        :param obj_type: "NODE", "LINK", or "SUBCATCH".
        :param obj_id: Object identifier.
        :param variable: Variable name (e.g., "max_depth").
        :returns: Statistic value.
        """
        cdef double v = 0.0
        cdef int rc = swmm_gpkg_read_summary(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'), &v)
        if rc != 0:
            raise KeyError(f"Summary not found: {obj_type}/{obj_id}/{variable}")
        return v

    # ------------------------------------------------------------------
    # Observed data write
    # ------------------------------------------------------------------

    def create_observed_series(self, str name, str variable,
                                str obj_type="", str obj_id="",
                                str source="", str units="") -> int:
        """Create an observed data series.

        :param name: Unique series name.
        :param variable: Variable being measured.
        :param obj_type: Model object type (for linking), or "".
        :param obj_id: Model object ID (for linking), or "".
        :param source: Data source description, or "".
        :param units: Measurement units, or "".
        :returns: Series ID (>= 0).
        """
        cdef int sid = swmm_gpkg_create_observed_series(
            self._handle,
            name.encode('utf-8'), variable.encode('utf-8'),
            obj_type.encode('utf-8') if obj_type else NULL,
            obj_id.encode('utf-8') if obj_id else NULL,
            source.encode('utf-8') if source else NULL,
            units.encode('utf-8') if units else NULL)
        if sid < 0:
            raise RuntimeError(f"Failed to create series: {self.last_error}")
        return sid

    def write_observed_value(self, int series_id, str timestamp,
                              double value, str flag=""):
        """Write a single observed data point.

        :param series_id: Series ID from create_observed_series().
        :param timestamp: ISO 8601 timestamp string.
        :param value: Measured value.
        :param flag: Quality flag (e.g., "A", "P"), or "".
        """
        cdef int rc = swmm_gpkg_write_observed_value(
            self._handle, series_id,
            timestamp.encode('utf-8'), value,
            flag.encode('utf-8') if flag else NULL)
        if rc != 0:
            raise RuntimeError(f"Write failed: {self.last_error}")

    def write_observed_values(self, int series_id, timestamps, values,
                               flags=None):
        """Bulk-write observed data points.

        For best performance, wrap in a transaction::

            gpkg.begin()
            gpkg.write_observed_values(sid, timestamps, values)
            gpkg.commit()

        :param series_id: Series ID.
        :param timestamps: List of ISO 8601 timestamp strings.
        :param values: List or numpy array of values.
        :param flags: Optional list of quality flag strings.
        """
        cdef int n = len(timestamps)
        cdef np.ndarray[double, ndim=1] vals = np.asarray(values, dtype=np.float64)

        # Build C string arrays
        cdef list ts_bytes = [t.encode('utf-8') for t in timestamps]
        cdef list fl_bytes = ([f.encode('utf-8') for f in flags]
                              if flags else [b""] * n)

        cdef const char** c_ts = <const char**>malloc(n * sizeof(char*))
        cdef const char** c_fl = <const char**>malloc(n * sizeof(char*))
        if c_ts is NULL or c_fl is NULL:
            if c_ts: free(c_ts)
            if c_fl: free(c_fl)
            raise MemoryError()

        try:
            for i in range(n):
                c_ts[i] = ts_bytes[i]
                c_fl[i] = fl_bytes[i]
            rc = swmm_gpkg_write_observed_values(
                self._handle, series_id,
                c_ts, <const double*>vals.data,
                c_fl if flags else NULL, n)
            if rc != 0:
                raise RuntimeError(f"Bulk write failed: {self.last_error}")
        finally:
            free(c_ts)
            free(c_fl)

    # ------------------------------------------------------------------
    # Observed data read
    # ------------------------------------------------------------------

    def observed_series_count(self) -> int:
        """Get the number of observed data series."""
        return swmm_gpkg_observed_series_count(self._handle)

    def observed_value_count(self, int series_id) -> int:
        """Get the number of values in an observed series."""
        return swmm_gpkg_observed_value_count(self._handle, series_id)

    def read_observed_values(self, int series_id):
        """Read observed timeseries values.

        :param series_id: Series ID.
        :returns: Tuple of (timestamps, values) where timestamps is a list
                  of strings and values is a numpy float64 array.
        """
        cdef int n = swmm_gpkg_observed_value_count(self._handle, series_id)
        if n <= 0:
            return [], np.empty(0, dtype=np.float64)

        cdef int ts_len = 32
        cdef bytearray ts_buf = bytearray(n * ts_len)
        cdef np.ndarray[double, ndim=1] values = np.empty(n, dtype=np.float64)

        cdef int read = swmm_gpkg_read_observed_values(
            self._handle, series_id,
            <char*>ts_buf, ts_len,
            <double*>values.data, n)

        timestamps = []
        for i in range(read):
            ts_raw = ts_buf[i * ts_len:(i + 1) * ts_len]
            ts_str = ts_raw.split(b'\x00')[0].decode('utf-8')
            timestamps.append(ts_str)

        return timestamps, values[:read]

    # ------------------------------------------------------------------
    # Ad-hoc queries
    # ------------------------------------------------------------------

    def query_int(self, str sql) -> int:
        """Execute a read-only SQL query and return the first integer result.

        :param sql: SELECT query string.
        :returns: Integer result.
        """
        return swmm_gpkg_query_int(self._handle, sql.encode('utf-8'))

    def query_double(self, str sql) -> float:
        """Execute a read-only SQL query and return the first double result.

        :param sql: SELECT query string.
        :returns: Double result.
        """
        cdef double v = 0.0
        cdef int rc = swmm_gpkg_query_double(self._handle,
                                              sql.encode('utf-8'), &v)
        if rc != 0:
            raise RuntimeError(f"Query failed: {self.last_error}")
        return v


# Module-level registration functions

def register(str key="", str org="", str email="", str deploy="") -> bool:
    """Register the GeoPackage plugin.

    :returns: True if registration succeeded.
    """
    return swmm_gpkg_register(
        key.encode('utf-8') if key else NULL,
        org.encode('utf-8') if org else NULL,
        email.encode('utf-8') if email else NULL,
        deploy.encode('utf-8') if deploy else NULL) != 0

def is_registered() -> bool:
    """Check whether the GeoPackage plugin is registered."""
    return swmm_gpkg_is_registered() != 0


# C memory management (for bulk write)
from libc.stdlib cimport malloc, free
