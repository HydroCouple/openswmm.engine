"""
GeoPackage Access
=================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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

    # Lifecycle — file I/O, marked nogil so the SQLite open/close
    # does not block the interpreter on another thread.
    SWMM_Gpkg swmm_gpkg_open(const char* path) nogil
    void swmm_gpkg_close(SWMM_Gpkg gpkg) nogil
    const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg)

    int swmm_gpkg_begin(SWMM_Gpkg gpkg) nogil
    int swmm_gpkg_commit(SWMM_Gpkg gpkg) nogil
    int swmm_gpkg_rollback(SWMM_Gpkg gpkg) nogil

    int swmm_gpkg_register(const char* key, const char* org,
                           const char* email, const char* deploy)
    int swmm_gpkg_is_registered()

    int swmm_gpkg_simulation_count(SWMM_Gpkg gpkg) nogil
    int swmm_gpkg_simulation_id(SWMM_Gpkg gpkg, int index, char* buf, int bufsz) nogil

    int swmm_gpkg_node_count(SWMM_Gpkg gpkg, const char* sim_id) nogil
    int swmm_gpkg_link_count(SWMM_Gpkg gpkg, const char* sim_id) nogil
    int swmm_gpkg_subcatch_count(SWMM_Gpkg gpkg, const char* sim_id) nogil
    int swmm_gpkg_gage_count(SWMM_Gpkg gpkg, const char* sim_id) nogil
    int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* sim_id) nogil
    int swmm_gpkg_variable_count(SWMM_Gpkg gpkg) nogil

    # Time-series read is the heaviest single call — a SQL query that
    # may return tens of thousands of rows. Definitely worth releasing
    # the GIL.
    int swmm_gpkg_result_ts_count(SWMM_Gpkg gpkg, const char* sim_id,
                                   const char* obj_type, const char* obj_id,
                                   const char* var_name) nogil
    int swmm_gpkg_read_result_ts(SWMM_Gpkg gpkg, const char* sim_id,
                                  const char* obj_type, const char* obj_id,
                                  const char* var_name,
                                  double* times, double* values, int max_count) nogil

    int swmm_gpkg_read_summary(SWMM_Gpkg gpkg, const char* sim_id,
                                const char* obj_type, const char* obj_id,
                                const char* var_name, double* value) nogil

    # Observed-data writers and readers — disk-backed SQL ops.
    int swmm_gpkg_create_observed_series(SWMM_Gpkg gpkg, const char* name,
                                          const char* var_name, const char* obj_type,
                                          const char* obj_id, const char* source,
                                          const char* units) nogil
    int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg, int series_id,
                                        const char* timestamp, double value,
                                        const char* quality_flag) nogil
    int swmm_gpkg_write_observed_values(SWMM_Gpkg gpkg, int series_id,
                                         const char** timestamps,
                                         const double* values,
                                         const char** quality_flags, int count) nogil

    int swmm_gpkg_observed_series_count(SWMM_Gpkg gpkg) nogil
    int swmm_gpkg_observed_value_count(SWMM_Gpkg gpkg, int series_id) nogil
    int swmm_gpkg_read_observed_values(SWMM_Gpkg gpkg, int series_id,
                                        char* timestamps, int ts_buf_len,
                                        double* values, int max_count) nogil

    int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql) nogil
    int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql, double* result) nogil


cdef class GeoPackage:
    """GeoPackage database access for SWMM models, results, and observed data.

    Wraps the C{swmm_gpkg_*} entry points of the OpenSWMM GeoPackage
    plugin. Supports the context-manager protocol for automatic resource
    cleanup::

        with GeoPackage("model.gpkg") as gpkg:
            print(gpkg.simulation_count())

    @ivar last_error: The most recent error message reported by the
        underlying GeoPackage library.
    """

    cdef SWMM_Gpkg _handle

    def __cinit__(self, str path):
        """Open a GeoPackage file.

        @param path: Path to the C{.gpkg} file.
        @type path: str
        @raise RuntimeError: If the file cannot be opened.
        """
        cdef bytes b = path.encode('utf-8')
        self._handle = swmm_gpkg_open(b)
        if self._handle is NULL:
            raise RuntimeError(f"Failed to open GeoPackage: {path}")

    def __dealloc__(self):
        """Release the underlying C{SWMM_Gpkg} handle when garbage-collected."""
        if self._handle is not NULL:
            swmm_gpkg_close(self._handle)
            self._handle = NULL

    # ====================================================================
    # File lifecycle (open/close)
    # ====================================================================

    def close(self):
        """Close the database connection.

        Calling C{close} more than once is harmless.
        """
        if self._handle is not NULL:
            swmm_gpkg_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        """Enter the context-manager scope.

        @return: This L{GeoPackage} instance.
        @rtype: GeoPackage
        """
        return self

    def __exit__(self, *args):
        """Close the database when leaving the context-manager scope.

        @param args: Standard C{(exc_type, exc_val, exc_tb)} tuple.
        @return: Always C{False} (do not suppress exceptions).
        @rtype: bool
        """
        self.close()
        return False

    @property
    def last_error(self) -> str:
        """The last error message from the GeoPackage library.

        @return: Error message string, or C{""} if no error has been
            reported.
        @rtype: str
        """
        cdef const char* msg = swmm_gpkg_last_error(self._handle)
        return msg.decode('utf-8') if msg else ""

    # ====================================================================
    # File lifecycle - transactions
    # ====================================================================

    def begin(self):
        """Begin a transaction for bulk operations.

        @raise RuntimeError: If the transaction cannot be started.
        """
        if swmm_gpkg_begin(self._handle) != 0:
            raise RuntimeError(f"Transaction begin failed: {self.last_error}")

    def commit(self):
        """Commit the current transaction.

        @raise RuntimeError: If the commit fails.
        """
        if swmm_gpkg_commit(self._handle) != 0:
            raise RuntimeError(f"Transaction commit failed: {self.last_error}")

    def rollback(self):
        """Roll back the current transaction.

        @raise RuntimeError: If the rollback fails.
        """
        if swmm_gpkg_rollback(self._handle) != 0:
            raise RuntimeError(f"Transaction rollback failed: {self.last_error}")

    # ====================================================================
    # Read operations - simulation metadata
    # ====================================================================

    def simulation_count(self) -> int:
        """Return the number of simulation runs in the file.

        @return: Simulation count.
        @rtype: int
        """
        return swmm_gpkg_simulation_count(self._handle)

    def simulation_ids(self) -> list:
        """Return all simulation IDs as a list of strings.

        @return: List of simulation identifier strings.
        @rtype: list
        """
        cdef int n = swmm_gpkg_simulation_count(self._handle)
        cdef char buf[256]
        result = []
        for i in range(n):
            if swmm_gpkg_simulation_id(self._handle, i, buf, 256) == 0:
                result.append(buf.decode('utf-8'))
        return result

    def object_counts(self, str sim_id) -> dict:
        """Return model object counts for a simulation.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @return: Dict with keys C{"nodes"}, C{"links"},
            C{"subcatchments"}, and C{"gages"}.
        @rtype: dict
        """
        cdef bytes b = sim_id.encode('utf-8')
        return {
            "nodes": swmm_gpkg_node_count(self._handle, b),
            "links": swmm_gpkg_link_count(self._handle, b),
            "subcatchments": swmm_gpkg_subcatch_count(self._handle, b),
            "gages": swmm_gpkg_gage_count(self._handle, b),
        }

    def variable_count(self) -> int:
        """Return the number of output variables defined.

        @return: Variable count.
        @rtype: int
        """
        return swmm_gpkg_variable_count(self._handle)

    def topology_edge_count(self, str sim_id) -> int:
        """Return the number of topology edges for a simulation.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @return: Topology edge count.
        @rtype: int
        """
        cdef bytes b = sim_id.encode('utf-8')
        return swmm_gpkg_topology_edge_count(self._handle, b)

    # ====================================================================
    # Read operations - result timeseries (zero-copy numpy)
    # ====================================================================

    def result_ts_count(self, str sim_id, str obj_type, str obj_id,
                        str variable) -> int:
        """Return the number of result timeseries records for a query.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, C{"SUBCATCH"},
            or C{"SYSTEM"}.
        @type obj_type: str
        @param obj_id: Object identifier (e.g. C{"J1"}).
        @type obj_id: str
        @param variable: Variable name (e.g. C{"depth"}, C{"flow"}).
        @type variable: str
        @return: Record count.
        @rtype: int
        """
        return swmm_gpkg_result_ts_count(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'))

    def read_result_ts(self, str sim_id, str obj_type, str obj_id,
                       str variable):
        """Read a result timeseries as NumPy arrays.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, C{"SUBCATCH"},
            or C{"SYSTEM"}.
        @type obj_type: str
        @param obj_id: Object identifier.
        @type obj_id: str
        @param variable: Variable name.
        @type variable: str
        @return: Tuple C{(times, values)} as NumPy C{float64} arrays.
        @rtype: tuple
        @see: L{result_ts_count}
        """
        cdef int n = swmm_gpkg_result_ts_count(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'))
        if n <= 0:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        cdef np.ndarray[double, ndim=1] times = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] values = np.empty(n, dtype=np.float64)
        # Re-encode strings outside the nogil block so the bytes objects
        # remain owned with the GIL held; then snapshot the raw pointers.
        cdef bytes b_sim = sim_id.encode('utf-8')
        cdef bytes b_otype = obj_type.encode('utf-8')
        cdef bytes b_oid = obj_id.encode('utf-8')
        cdef bytes b_var = variable.encode('utf-8')
        cdef SWMM_Gpkg gpkg = self._handle
        cdef const char* p_sim = b_sim
        cdef const char* p_otype = b_otype
        cdef const char* p_oid = b_oid
        cdef const char* p_var = b_var
        cdef double* p_times = <double*>times.data
        cdef double* p_values = <double*>values.data
        cdef int read
        # SQL execution can take many milliseconds for large series — worth
        # releasing the GIL so another thread can do useful work meanwhile.
        with nogil:
            read = swmm_gpkg_read_result_ts(
                gpkg, p_sim, p_otype, p_oid, p_var,
                p_times, p_values, n)
        return times[:read], values[:read]

    # ====================================================================
    # Read operations - summary statistics
    # ====================================================================

    def read_summary(self, str sim_id, str obj_type, str obj_id,
                     str variable) -> float:
        """Read a single summary statistic value.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, or C{"SUBCATCH"}.
        @type obj_type: str
        @param obj_id: Object identifier.
        @type obj_id: str
        @param variable: Variable name (e.g. C{"max_depth"}).
        @type variable: str
        @return: Statistic value.
        @rtype: float
        @raise KeyError: If the summary record is not found.
        """
        cdef double v = 0.0
        cdef int rc = swmm_gpkg_read_summary(
            self._handle, sim_id.encode('utf-8'),
            obj_type.encode('utf-8'), obj_id.encode('utf-8'),
            variable.encode('utf-8'), &v)
        if rc != 0:
            raise KeyError(f"Summary not found: {obj_type}/{obj_id}/{variable}")
        return v

    # ====================================================================
    # Write operations - observed data
    # ====================================================================

    def create_observed_series(self, str name, str variable,
                                str obj_type="", str obj_id="",
                                str source="", str units="") -> int:
        """Create an observed-data series.

        @param name: Unique series name.
        @type name: str
        @param variable: Variable being measured.
        @type variable: str
        @param obj_type: Model object type for linking, or C{""} for
            none.
        @type obj_type: str
        @param obj_id: Model object ID for linking, or C{""} for none.
        @type obj_id: str
        @param source: Data source description, or C{""} for none.
        @type source: str
        @param units: Measurement units, or C{""} for none.
        @type units: str
        @return: Series ID (M{>= 0}).
        @rtype: int
        @raise RuntimeError: If the series cannot be created.
        """
        # NOTE: keep the encoded bytes alive in locals before extracting the
        # raw `const char*` — under Cython >= 3.0.12 the conditional
        # expression `x.encode('utf-8') if x else NULL` cannot unify
        # `bytes` and `void*` and fails to transpile (see plan §Appendix A).
        cdef bytes b_name = name.encode('utf-8')
        cdef bytes b_var = variable.encode('utf-8')
        cdef bytes b_otype = obj_type.encode('utf-8') if obj_type else b""
        cdef bytes b_oid = obj_id.encode('utf-8') if obj_id else b""
        cdef bytes b_src = source.encode('utf-8') if source else b""
        cdef bytes b_units = units.encode('utf-8') if units else b""
        cdef const char* p_otype = <const char*>b_otype if obj_type else NULL
        cdef const char* p_oid = <const char*>b_oid if obj_id else NULL
        cdef const char* p_src = <const char*>b_src if source else NULL
        cdef const char* p_units = <const char*>b_units if units else NULL
        cdef int sid = swmm_gpkg_create_observed_series(
            self._handle,
            b_name, b_var,
            p_otype, p_oid, p_src, p_units)
        if sid < 0:
            raise RuntimeError(f"Failed to create series: {self.last_error}")
        return sid

    def write_observed_value(self, int series_id, str timestamp,
                              double value, str flag=""):
        """Write a single observed data point.

        @param series_id: Series ID returned by
            L{create_observed_series}.
        @type series_id: int
        @param timestamp: ISO 8601 timestamp string.
        @type timestamp: str
        @param value: Measured value.
        @type value: float
        @param flag: Quality flag (e.g. C{"A"}, C{"P"}), or C{""} for
            none.
        @type flag: str
        @raise RuntimeError: If the write fails.
        """
        # See create_observed_series above for the encode/NULL pattern
        # rationale.
        cdef bytes b_ts = timestamp.encode('utf-8')
        cdef bytes b_flag = flag.encode('utf-8') if flag else b""
        cdef const char* p_flag = <const char*>b_flag if flag else NULL
        cdef int rc = swmm_gpkg_write_observed_value(
            self._handle, series_id,
            b_ts, value, p_flag)
        if rc != 0:
            raise RuntimeError(f"Write failed: {self.last_error}")

    def write_observed_values(self, int series_id, timestamps, values,
                               flags=None):
        """Bulk-write observed data points.

        For best performance wrap the call in a transaction::

            gpkg.begin()
            gpkg.write_observed_values(sid, timestamps, values)
            gpkg.commit()

        @param series_id: Series ID.
        @type series_id: int
        @param timestamps: List of ISO 8601 timestamp strings.
        @type timestamps: list
        @param values: List or NumPy array of values.
        @type values: np.ndarray
        @param flags: Optional list of quality-flag strings, one per
            timestamp.
        @type flags: list
        @raise RuntimeError: If the bulk write fails.
        @raise MemoryError: If the C string-pointer arrays cannot be
            allocated.
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

        cdef SWMM_Gpkg gpkg = self._handle
        cdef const double* p_vals = <const double*>vals.data
        cdef const char** c_fl_arg
        cdef int rc_c
        try:
            for i in range(n):
                c_ts[i] = ts_bytes[i]
                c_fl[i] = fl_bytes[i]
            c_fl_arg = c_fl if flags else NULL
            # Bulk SQL inserts can take a long time for large series;
            # release the GIL so peer threads continue.
            with nogil:
                rc_c = swmm_gpkg_write_observed_values(
                    gpkg, series_id, c_ts, p_vals, c_fl_arg, n)
            if rc_c != 0:
                raise RuntimeError(f"Bulk write failed: {self.last_error}")
        finally:
            free(c_ts)
            free(c_fl)

    # ====================================================================
    # Read operations - observed data
    # ====================================================================

    def observed_series_count(self) -> int:
        """Return the number of observed-data series.

        @return: Observed-series count.
        @rtype: int
        """
        return swmm_gpkg_observed_series_count(self._handle)

    def observed_value_count(self, int series_id) -> int:
        """Return the number of values in an observed series.

        @param series_id: Series ID.
        @type series_id: int
        @return: Value count.
        @rtype: int
        """
        return swmm_gpkg_observed_value_count(self._handle, series_id)

    def read_observed_values(self, int series_id):
        """Read observed-timeseries values.

        @param series_id: Series ID.
        @type series_id: int
        @return: Tuple C{(timestamps, values)} where C{timestamps} is a
            list of ISO 8601 strings and C{values} is a NumPy
            C{float64} array.
        @rtype: tuple
        """
        cdef int n = swmm_gpkg_observed_value_count(self._handle, series_id)
        if n <= 0:
            return [], np.empty(0, dtype=np.float64)

        cdef int ts_len = 32
        cdef bytearray ts_buf = bytearray(n * ts_len)
        cdef np.ndarray[double, ndim=1] values = np.empty(n, dtype=np.float64)
        cdef SWMM_Gpkg gpkg = self._handle
        cdef char* p_ts = <char*>ts_buf
        cdef double* p_v = <double*>values.data
        cdef int read
        with nogil:
            read = swmm_gpkg_read_observed_values(
                gpkg, series_id, p_ts, ts_len, p_v, n)

        timestamps = []
        for i in range(read):
            ts_raw = ts_buf[i * ts_len:(i + 1) * ts_len]
            ts_str = ts_raw.split(b'\x00')[0].decode('utf-8')
            timestamps.append(ts_str)

        return timestamps, values[:read]

    # ====================================================================
    # Layer queries (ad-hoc SQL)
    # ====================================================================

    def query_int(self, str sql) -> int:
        """Execute a read-only SQL query and return the first integer result.

        @param sql: C{SELECT} query string.
        @type sql: str
        @return: Integer result of the query.
        @rtype: int
        """
        return swmm_gpkg_query_int(self._handle, sql.encode('utf-8'))

    def query_double(self, str sql) -> float:
        """Execute a read-only SQL query and return the first double result.

        @param sql: C{SELECT} query string.
        @type sql: str
        @return: Double-precision result of the query.
        @rtype: float
        @raise RuntimeError: If the query fails.
        """
        cdef double v = 0.0
        cdef int rc = swmm_gpkg_query_double(self._handle,
                                              sql.encode('utf-8'), &v)
        if rc != 0:
            raise RuntimeError(f"Query failed: {self.last_error}")
        return v


# ====================================================================
# Schema queries - module-level registration
# ====================================================================

def register(str key="", str org="", str email="", str deploy="") -> bool:
    """Register the GeoPackage plugin.

    @param key: License key, or C{""} if not required.
    @type key: str
    @param org: Organisation name, or C{""}.
    @type org: str
    @param email: Contact e-mail, or C{""}.
    @type email: str
    @param deploy: Deployment identifier, or C{""}.
    @type deploy: str
    @return: C{True} if registration succeeded.
    @rtype: bool
    """
    # See GeoPackage.create_observed_series for the encode/NULL pattern
    # rationale (Cython >= 3.0.12 cannot unify bytes / void* in a conditional).
    cdef bytes b_key = key.encode('utf-8') if key else b""
    cdef bytes b_org = org.encode('utf-8') if org else b""
    cdef bytes b_email = email.encode('utf-8') if email else b""
    cdef bytes b_deploy = deploy.encode('utf-8') if deploy else b""
    cdef const char* p_key = <const char*>b_key if key else NULL
    cdef const char* p_org = <const char*>b_org if org else NULL
    cdef const char* p_email = <const char*>b_email if email else NULL
    cdef const char* p_deploy = <const char*>b_deploy if deploy else NULL
    return swmm_gpkg_register(p_key, p_org, p_email, p_deploy) != 0

def is_registered() -> bool:
    """Check whether the GeoPackage plugin is registered.

    @return: C{True} if registered.
    @rtype: bool
    """
    return swmm_gpkg_is_registered() != 0


# C memory management (for bulk write)
from libc.stdlib cimport malloc, free
