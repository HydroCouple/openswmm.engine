"""
Simulation Statistics
=====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Statistics` class provides access to post-simulation summary
statistics for nodes, links, and subcatchments.

.. code-block:: python

    from openswmm.engine import Solver
    from openswmm.engine._statistics import Statistics

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.step():
            pass
        stats = Statistics(s)
        print(f"Max depth at node 0: {stats.node_max_depth(0)}")
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Statistics:
    """Access post-simulation summary statistics.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Node statistics
    # ------------------------------------------------------------------

    def node_max_depth(self, int idx) -> float:
        """Return the maximum depth recorded at a node.

        :param idx: Node index.
        :returns: Maximum depth.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_depth(h, idx, &v))
        return v

    def node_max_overflow(self, int idx) -> float:
        """Return the maximum overflow rate recorded at a node.

        :param idx: Node index.
        :returns: Maximum overflow rate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_overflow(h, idx, &v))
        return v

    def node_vol_flooded(self, int idx) -> float:
        """Return the total volume flooded at a node.

        :param idx: Node index.
        :returns: Volume flooded.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_vol_flooded(h, idx, &v))
        return v

    def node_time_flooded(self, int idx) -> float:
        """Return the total time a node was flooded.

        :param idx: Node index.
        :returns: Time flooded (hours).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_time_flooded(h, idx, &v))
        return v

    # ------------------------------------------------------------------
    # Link statistics
    # ------------------------------------------------------------------

    def link_max_flow(self, int idx) -> float:
        """Return the maximum flow recorded in a link.

        :param idx: Link index.
        :returns: Maximum flow.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_flow(h, idx, &v))
        return v

    def link_max_velocity(self, int idx) -> float:
        """Return the maximum velocity recorded in a link.

        :param idx: Link index.
        :returns: Maximum velocity.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_velocity(h, idx, &v))
        return v

    def link_max_filling(self, int idx) -> float:
        """Return the maximum filling fraction recorded in a link.

        :param idx: Link index.
        :returns: Maximum filling fraction.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_filling(h, idx, &v))
        return v

    def link_vol_flow(self, int idx) -> float:
        """Return the total volume of flow through a link.

        :param idx: Link index.
        :returns: Total flow volume.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_vol_flow(h, idx, &v))
        return v

    def link_surcharge_time(self, int idx) -> float:
        """Return the total surcharge time for a link.

        :param idx: Link index.
        :returns: Surcharge time (hours).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_surcharge_time(h, idx, &v))
        return v

    # ------------------------------------------------------------------
    # Subcatchment statistics
    # ------------------------------------------------------------------

    def subcatch_precip(self, int idx) -> float:
        """Return the total precipitation on a subcatchment.

        :param idx: Subcatchment index.
        :returns: Total precipitation volume.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_precip(h, idx, &v))
        return v

    def subcatch_runoff_vol(self, int idx) -> float:
        """Return the total runoff volume from a subcatchment.

        :param idx: Subcatchment index.
        :returns: Total runoff volume.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_runoff_vol(h, idx, &v))
        return v

    def subcatch_max_runoff(self, int idx) -> float:
        """Return the maximum runoff rate from a subcatchment.

        :param idx: Subcatchment index.
        :returns: Maximum runoff rate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_max_runoff(h, idx, &v))
        return v

    # ------------------------------------------------------------------
    # Bulk statistics (numpy)
    # ------------------------------------------------------------------

    def node_max_depth_bulk(self) -> np.ndarray:
        """Return maximum depths for all nodes as a NumPy array.

        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_node_max_depth_bulk(h, <double*>buf.data, n))
        return buf

    def link_max_flow_bulk(self) -> np.ndarray:
        """Return maximum flows for all links as a NumPy array.

        :returns: Array of shape ``(n_links,)`` with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_link_max_flow_bulk(h, <double*>buf.data, n))
        return buf

    def subcatch_runoff_vol_bulk(self) -> np.ndarray:
        """Return total runoff volumes for all subcatchments as a NumPy array.

        :returns: Array of shape ``(n_subcatchments,)`` with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_subcatch_runoff_vol_bulk(h, <double*>buf.data, n))
        return buf
