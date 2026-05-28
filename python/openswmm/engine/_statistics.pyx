"""
Simulation statistics (Pythonic v1 surface)
===========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

This module exposes the **bulk** views of the per-object cumulative
statistics. Single-object access lives on the wrapper classes
(:attr:`Node.stats`, :attr:`Link.stats`, :attr:`Subcatchment.stats`),
so this view is the right tool when you want a numpy array of every
node's flooded volume in one C call.

.. code-block:: python

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass
        stats = s.statistics
        vol_flooded   = stats.node_vol_flooded         # np.ndarray(n_nodes)
        max_flow      = stats.link_max_flow            # np.ndarray(n_links)
        runoff_vol    = stats.subcatchment_runoff_vol  # np.ndarray(n_subcatch)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


class Statistics:
    """Bulk per-object cumulative statistics. Reach via ``solver.statistics``.

    Each property returns a fresh ``float64`` numpy array of length
    matching the domain count.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Node bulk
    # ------------------------------------------------------------------

    @property
    def node_max_depth(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_node_max_depth_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_max_overflow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_node_max_overflow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_vol_flooded(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_node_vol_flooded_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_time_flooded(self):
        """Cumulative flooded duration per node (seconds)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_node_time_flooded_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Link bulk
    # ------------------------------------------------------------------

    @property
    def link_max_flow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_link_max_flow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_max_velocity(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_link_max_velocity_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_max_filling(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_link_max_filling_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_vol_flow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_link_vol_flow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_surcharge_time(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_link_surcharge_time_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Subcatchment bulk
    # ------------------------------------------------------------------

    @property
    def subcatchment_runoff_vol(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_subcatch_runoff_vol_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def subcatchment_max_runoff(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_stat_subcatch_max_runoff_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    def __repr__(self) -> str:
        return f"<Statistics for {self._solver!r}>"
