"""
Binary Output File Reader
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`OutputReader` class reads SWMM binary output files
(``.out``) independently of the engine handle.

.. code-block:: python

    from openswmm.engine._output_reader import OutputReader

    with OutputReader("model.out") as out:
        print(f"Periods: {out.get_period_count()}")
        depths = out.get_node_result(0, 0)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


cdef class OutputReader:
    """Read a SWMM binary output file.

    Supports the context manager protocol for automatic resource cleanup.
    Operates on the binary C{.out} file produced by a completed simulation
    and does not require an active engine handle.

    @ivar _handle: Opaque pointer to the underlying C{SWMM_Output}
        handle. Managed internally; not part of the public API.
    """

    cdef SWMM_Output _handle

    def __init__(self, str path):
        """Open a SWMM binary output file.

        Wraps C{swmm_output_open}.

        @param path: Path to the C{.out} file.
        @type path: str
        @raise IOError: If the file cannot be opened.
        """
        cdef bytes b = path.encode('utf-8')
        self._handle = swmm_output_open(b)
        if self._handle == NULL:
            raise IOError(f"Cannot open output file: {path}")

    # ====================================================================
    # File lifecycle (open / close)
    # ====================================================================

    def close(self):
        """Close the output file and release resources.

        Wraps C{swmm_output_close}. Safe to call multiple times.

        @return: C{None}.
        @rtype: NoneType
        """
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        """Enter the context manager.

        @return: This L{OutputReader} instance.
        @rtype: OutputReader
        """
        return self

    def __exit__(self, *args):
        """Exit the context manager and L{close} the file.

        @param args: Standard C{(exc_type, exc_val, exc_tb)} tuple.
        @type args: tuple
        @return: C{False} so any in-flight exception propagates.
        @rtype: bool
        """
        self.close()
        return False

    def __dealloc__(self):
        """Free the underlying C handle on garbage collection.

        Internal Cython destructor; do not call directly.
        """
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    # ====================================================================
    # Metadata queries
    # ====================================================================

    def get_version(self) -> int:
        """Return the SWMM version that produced the output file.

        Wraps C{swmm_output_get_version}.

        @return: Version integer.
        @rtype: int
        """
        return swmm_output_get_version(self._handle)

    def get_flow_units(self) -> int:
        """Return the flow units code.

        Wraps C{swmm_output_get_flow_units}.

        @return: Flow units code (see L{openswmm.engine.FlowUnits}).
        @rtype: int
        @see: L{openswmm.engine.FlowUnits}
        """
        return swmm_output_get_flow_units(self._handle)

    def get_subcatch_count(self) -> int:
        """Return the number of subcatchments in the output file.

        Wraps C{swmm_output_get_subcatch_count}.

        @return: Number of subcatchments.
        @rtype: int
        """
        return swmm_output_get_subcatch_count(self._handle)

    def get_node_count(self) -> int:
        """Return the number of nodes in the output file.

        Wraps C{swmm_output_get_node_count}.

        @return: Number of nodes.
        @rtype: int
        """
        return swmm_output_get_node_count(self._handle)

    def get_link_count(self) -> int:
        """Return the number of links in the output file.

        Wraps C{swmm_output_get_link_count}.

        @return: Number of links.
        @rtype: int
        """
        return swmm_output_get_link_count(self._handle)

    def get_pollut_count(self) -> int:
        """Return the number of pollutants in the output file.

        Wraps C{swmm_output_get_pollut_count}.

        @return: Number of pollutants.
        @rtype: int
        """
        return swmm_output_get_pollut_count(self._handle)

    def get_subcatch_id(self, int index) -> str:
        """Return the ID string of a subcatchment by index.

        Wraps C{swmm_output_get_subcatch_id}.

        @param index: Zero-based subcatchment index.
        @type index: int
        @return: Subcatchment identifier (UTF-8 decoded), or empty string
            if not available.
        @rtype: str
        """
        cdef const char* s = swmm_output_get_subcatch_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def get_node_id(self, int index) -> str:
        """Return the ID string of a node by index.

        Wraps C{swmm_output_get_node_id}.

        @param index: Zero-based node index.
        @type index: int
        @return: Node identifier (UTF-8 decoded), or empty string if not
            available.
        @rtype: str
        """
        cdef const char* s = swmm_output_get_node_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def get_link_id(self, int index) -> str:
        """Return the ID string of a link by index.

        Wraps C{swmm_output_get_link_id}.

        @param index: Zero-based link index.
        @type index: int
        @return: Link identifier (UTF-8 decoded), or empty string if not
            available.
        @rtype: str
        """
        cdef const char* s = swmm_output_get_link_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    # ====================================================================
    # Time / period queries
    # ====================================================================

    def get_period_count(self) -> int:
        """Return the number of reporting periods.

        Wraps C{swmm_output_get_period_count}.

        @return: Number of reporting periods.
        @rtype: int
        """
        return swmm_output_get_period_count(self._handle)

    def get_start_date(self) -> float:
        """Return the simulation start date as a Julian date value.

        Wraps C{swmm_output_get_start_date}.

        @return: Julian date.
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        """
        cdef double v = 0.0
        cdef int rc = swmm_output_get_start_date(self._handle, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v

    def get_report_step(self) -> int:
        """Return the reporting time step in seconds.

        Wraps C{swmm_output_get_report_step}.

        @return: Reporting time step (seconds).
        @rtype: int
        """
        return swmm_output_get_report_step(self._handle)

    def get_error_code(self) -> int:
        """Return the error code stored in the output file.

        Wraps C{swmm_output_get_error_code}.

        @return: Error code (C{0} on success).
        @rtype: int
        """
        return swmm_output_get_error_code(self._handle)

    def get_period_time(self, int period) -> float:
        """Return the elapsed time for a reporting period.

        Wraps C{swmm_output_get_period_time}.

        @param period: Zero-based reporting period index.
        @type period: int
        @return: Elapsed time value (project time units).
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        """
        cdef double v = 0.0
        cdef int rc = swmm_output_get_period_time(self._handle, period, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v

    # ====================================================================
    # Time series extraction (per element type): subcatchments
    # ====================================================================

    def get_subcatch_result(self, int period, int var) -> np.ndarray:
        """Return a subcatchment variable for all subcatchments at a period.

        Wraps C{swmm_output_get_subcatch_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutSubcatchVar}).
        @type var: int
        @return: Array of shape C{(n_subcatchments,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutSubcatchVar}
        """
        cdef int n = swmm_output_get_subcatch_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_subcatch_series(self, int subcatch_idx, int var,
                            int start, int end) -> np.ndarray:
        """Return a time series of a subcatchment variable.

        Wraps C{swmm_output_get_subcatch_series}.

        @param subcatch_idx: Zero-based subcatchment index.
        @type subcatch_idx: int
        @param var: Variable code (see L{openswmm.engine.OutSubcatchVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_series(
            self._handle, subcatch_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_subcatch_attribute(self, int subcatch_idx, int period) -> np.ndarray:
        """Return all attribute values for a subcatchment at a period.

        Wraps C{swmm_output_get_subcatch_attribute}.

        @param subcatch_idx: Zero-based subcatchment index.
        @type subcatch_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_attribute(
            self._handle, subcatch_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    # ====================================================================
    # Time series extraction (per element type): nodes
    # ====================================================================

    def get_node_result(self, int period, int var) -> np.ndarray:
        """Return a node variable for all nodes at a period.

        Wraps C{swmm_output_get_node_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutNodeVar}).
        @type var: int
        @return: Array of shape C{(n_nodes,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutNodeVar}
        """
        cdef int n = swmm_output_get_node_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_node_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_node_series(self, int node_idx, int var,
                        int start, int end) -> np.ndarray:
        """Return a time series of a node variable.

        Wraps C{swmm_output_get_node_series}.

        @param node_idx: Zero-based node index.
        @type node_idx: int
        @param var: Variable code (see L{openswmm.engine.OutNodeVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_node_series(
            self._handle, node_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_node_attribute(self, int node_idx, int period) -> np.ndarray:
        """Return all attribute values for a node at a period.

        Wraps C{swmm_output_get_node_attribute}.

        @param node_idx: Zero-based node index.
        @type node_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_node_attribute(
            self._handle, node_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    # ====================================================================
    # Time series extraction (per element type): links
    # ====================================================================

    def get_link_result(self, int period, int var) -> np.ndarray:
        """Return a link variable for all links at a period.

        Wraps C{swmm_output_get_link_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutLinkVar}).
        @type var: int
        @return: Array of shape C{(n_links,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutLinkVar}
        """
        cdef int n = swmm_output_get_link_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_link_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_link_series(self, int link_idx, int var,
                        int start, int end) -> np.ndarray:
        """Return a time series of a link variable.

        Wraps C{swmm_output_get_link_series}.

        @param link_idx: Zero-based link index.
        @type link_idx: int
        @param var: Variable code (see L{openswmm.engine.OutLinkVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_link_series(
            self._handle, link_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_link_attribute(self, int link_idx, int period) -> np.ndarray:
        """Return all attribute values for a link at a period.

        Wraps C{swmm_output_get_link_attribute}.

        @param link_idx: Zero-based link index.
        @type link_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_link_attribute(
            self._handle, link_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    # ====================================================================
    # Time series extraction (per element type): system
    # ====================================================================

    def get_system_result(self, int period, int var) -> float:
        """Return a system-level variable at a period.

        Wraps C{swmm_output_get_system_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutSystemVar}).
        @type var: int
        @return: Variable value.
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutSystemVar}
        """
        cdef float v = 0.0
        cdef int rc = swmm_output_get_system_result(
            self._handle, period, var, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v

    def get_system_series(self, int var, int start, int end) -> np.ndarray:
        """Return a time series of a system-level variable.

        Wraps C{swmm_output_get_system_series}.

        @param var: Variable code (see L{openswmm.engine.OutSystemVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_system_series(
            self._handle, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf
