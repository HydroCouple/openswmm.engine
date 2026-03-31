"""
Binary Output File Reader
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    :param path: Path to the ``.out`` file.
    :raises IOError: If the file cannot be opened.
    """

    cdef SWMM_Output _handle

    def __init__(self, str path):
        cdef bytes b = path.encode('utf-8')
        self._handle = swmm_output_open(b)
        if self._handle == NULL:
            raise IOError(f"Cannot open output file: {path}")

    def close(self):
        """Close the output file and release resources."""
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    def __dealloc__(self):
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    # ------------------------------------------------------------------
    # Metadata
    # ------------------------------------------------------------------

    def get_version(self) -> int:
        """Return the SWMM version that produced the output file."""
        return swmm_output_get_version(self._handle)

    def get_flow_units(self) -> int:
        """Return the flow units code."""
        return swmm_output_get_flow_units(self._handle)

    def get_subcatch_count(self) -> int:
        """Return the number of subcatchments."""
        return swmm_output_get_subcatch_count(self._handle)

    def get_node_count(self) -> int:
        """Return the number of nodes."""
        return swmm_output_get_node_count(self._handle)

    def get_link_count(self) -> int:
        """Return the number of links."""
        return swmm_output_get_link_count(self._handle)

    def get_pollut_count(self) -> int:
        """Return the number of pollutants."""
        return swmm_output_get_pollut_count(self._handle)

    def get_period_count(self) -> int:
        """Return the number of reporting periods."""
        return swmm_output_get_period_count(self._handle)

    def get_start_date(self) -> float:
        """Return the simulation start date as a Julian date value."""
        cdef double v = 0.0
        cdef int rc = swmm_output_get_start_date(self._handle, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v

    def get_report_step(self) -> int:
        """Return the reporting time step in seconds."""
        return swmm_output_get_report_step(self._handle)

    def get_error_code(self) -> int:
        """Return the error code stored in the output file."""
        return swmm_output_get_error_code(self._handle)

    # ------------------------------------------------------------------
    # Object IDs
    # ------------------------------------------------------------------

    def get_subcatch_id(self, int index) -> str:
        """Return the ID string of a subcatchment by index.

        :param index: Subcatchment index.
        :returns: Subcatchment identifier.
        """
        cdef const char* s = swmm_output_get_subcatch_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def get_node_id(self, int index) -> str:
        """Return the ID string of a node by index.

        :param index: Node index.
        :returns: Node identifier.
        """
        cdef const char* s = swmm_output_get_node_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def get_link_id(self, int index) -> str:
        """Return the ID string of a link by index.

        :param index: Link index.
        :returns: Link identifier.
        """
        cdef const char* s = swmm_output_get_link_id(self._handle, index)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    # ------------------------------------------------------------------
    # Per-period results
    # ------------------------------------------------------------------

    def get_subcatch_result(self, int period, int var) -> np.ndarray:
        """Return a subcatchment variable for all subcatchments at a period.

        :param period: Reporting period index.
        :param var: Variable code.
        :returns: Array of shape ``(n_subcatchments,)`` with dtype ``float32``.
        """
        cdef int n = swmm_output_get_subcatch_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_node_result(self, int period, int var) -> np.ndarray:
        """Return a node variable for all nodes at a period.

        :param period: Reporting period index.
        :param var: Variable code.
        :returns: Array of shape ``(n_nodes,)`` with dtype ``float32``.
        """
        cdef int n = swmm_output_get_node_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_node_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_link_result(self, int period, int var) -> np.ndarray:
        """Return a link variable for all links at a period.

        :param period: Reporting period index.
        :param var: Variable code.
        :returns: Array of shape ``(n_links,)`` with dtype ``float32``.
        """
        cdef int n = swmm_output_get_link_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_link_result(
            self._handle, period, var, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_system_result(self, int period, int var) -> float:
        """Return a system-level variable at a period.

        :param period: Reporting period index.
        :param var: Variable code.
        :returns: Variable value.
        """
        cdef float v = 0.0
        cdef int rc = swmm_output_get_system_result(
            self._handle, period, var, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v

    # ------------------------------------------------------------------
    # Time series
    # ------------------------------------------------------------------

    def get_subcatch_series(self, int subcatch_idx, int var,
                            int start, int end) -> np.ndarray:
        """Return a time series of a subcatchment variable.

        :param subcatch_idx: Subcatchment index.
        :param var: Variable code.
        :param start: Start period index.
        :param end: End period index (inclusive).
        :returns: Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_series(
            self._handle, subcatch_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_node_series(self, int node_idx, int var,
                        int start, int end) -> np.ndarray:
        """Return a time series of a node variable.

        :param node_idx: Node index.
        :param var: Variable code.
        :param start: Start period index.
        :param end: End period index (inclusive).
        :returns: Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_node_series(
            self._handle, node_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_link_series(self, int link_idx, int var,
                        int start, int end) -> np.ndarray:
        """Return a time series of a link variable.

        :param link_idx: Link index.
        :param var: Variable code.
        :param start: Start period index.
        :param end: End period index (inclusive).
        :returns: Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_link_series(
            self._handle, link_idx, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    def get_system_series(self, int var, int start, int end) -> np.ndarray:
        """Return a time series of a system-level variable.

        :param var: Variable code.
        :param start: Start period index.
        :param end: End period index (inclusive).
        :returns: Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        cdef int n = end - start + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_system_series(
            self._handle, var, start, end, <float*>buf.data)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf

    # ------------------------------------------------------------------
    # Per-object attribute
    # ------------------------------------------------------------------

    def get_subcatch_attribute(self, int subcatch_idx, int period) -> np.ndarray:
        """Return all attribute values for a subcatchment at a period.

        :param subcatch_idx: Subcatchment index.
        :param period: Reporting period index.
        :returns: Array of attribute values with dtype ``float32``.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_attribute(
            self._handle, subcatch_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    def get_node_attribute(self, int node_idx, int period) -> np.ndarray:
        """Return all attribute values for a node at a period.

        :param node_idx: Node index.
        :param period: Reporting period index.
        :returns: Array of attribute values with dtype ``float32``.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_node_attribute(
            self._handle, node_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    def get_link_attribute(self, int link_idx, int period) -> np.ndarray:
        """Return all attribute values for a link at a period.

        :param link_idx: Link index.
        :param period: Reporting period index.
        :returns: Array of attribute values with dtype ``float32``.
        """
        cdef int count = 0
        cdef np.ndarray[float, ndim=1] buf = np.empty(32, dtype=np.float32)
        cdef int rc = swmm_output_get_link_attribute(
            self._handle, link_idx, period, <float*>buf.data, &count)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return buf[:count]

    # ------------------------------------------------------------------
    # Time
    # ------------------------------------------------------------------

    def get_period_time(self, int period) -> float:
        """Return the elapsed time for a reporting period.

        :param period: Reporting period index.
        :returns: Elapsed time value.
        """
        cdef double v = 0.0
        cdef int rc = swmm_output_get_period_time(self._handle, period, &v)
        if rc != 0:
            raise RuntimeError(f"Output read error {rc}")
        return v
