"""
Simulation Statistics
=====================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Statistics` class provides access to post-simulation summary
statistics for nodes, links, and subcatchments.

.. code-block:: python

    from openswmm.engine import Solver
    from openswmm.engine._statistics import Statistics

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
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

    Query after calling L{Solver.end} to obtain cumulative extremes and
    totals computed over the full simulation. The L{Solver} must remain
    alive for the lifetime of this object.

    @ivar _solver: The L{Solver} instance providing the engine handle.
    """

    def __init__(self, solver):
        """Construct a L{Statistics} bound to a solver.

        @param solver: An active L{Solver} instance. The solver must
            remain alive for the lifetime of this object.
        @type solver: Solver
        """
        self._solver = solver

    # ====================================================================
    # Per-element statistics: nodes
    # ====================================================================

    def node_max_depth(self, int idx) -> float:
        """Return the maximum depth recorded at a node.

        Wraps C{swmm_stat_node_max_depth}.

        @param idx: Zero-based node index.
        @type idx: int
        @return: Maximum depth (project length units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_depth(h, idx, &v))
        return v

    def node_max_overflow(self, int idx) -> float:
        """Return the maximum overflow rate recorded at a node.

        Wraps C{swmm_stat_node_max_overflow}.

        @param idx: Zero-based node index.
        @type idx: int
        @return: Maximum overflow rate (project flow units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_overflow(h, idx, &v))
        return v

    def node_vol_flooded(self, int idx) -> float:
        """Return the total volume flooded at a node.

        Wraps C{swmm_stat_node_vol_flooded}.

        @param idx: Zero-based node index.
        @type idx: int
        @return: Volume flooded (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_vol_flooded(h, idx, &v))
        return v

    def node_time_flooded(self, int idx) -> float:
        """Return the total time a node was flooded.

        Wraps C{swmm_stat_node_time_flooded}.

        @param idx: Zero-based node index.
        @type idx: int
        @return: Time flooded (hours).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_time_flooded(h, idx, &v))
        return v

    # ====================================================================
    # Per-element statistics: links
    # ====================================================================

    def link_max_flow(self, int idx) -> float:
        """Return the maximum flow recorded in a link.

        Wraps C{swmm_stat_link_max_flow}.

        @param idx: Zero-based link index.
        @type idx: int
        @return: Maximum flow (project flow units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_flow(h, idx, &v))
        return v

    def link_max_velocity(self, int idx) -> float:
        """Return the maximum velocity recorded in a link.

        Wraps C{swmm_stat_link_max_velocity}.

        @param idx: Zero-based link index.
        @type idx: int
        @return: Maximum velocity (project length / time units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_velocity(h, idx, &v))
        return v

    def link_max_filling(self, int idx) -> float:
        """Return the maximum filling fraction recorded in a link.

        Wraps C{swmm_stat_link_max_filling}.

        @param idx: Zero-based link index.
        @type idx: int
        @return: Maximum filling fraction (C{0}-C{1}).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_filling(h, idx, &v))
        return v

    def link_vol_flow(self, int idx) -> float:
        """Return the total volume of flow through a link.

        Wraps C{swmm_stat_link_vol_flow}.

        @param idx: Zero-based link index.
        @type idx: int
        @return: Total flow volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_vol_flow(h, idx, &v))
        return v

    def link_surcharge_time(self, int idx) -> float:
        """Return the total surcharge time for a link.

        Wraps C{swmm_stat_link_surcharge_time}.

        @param idx: Zero-based link index.
        @type idx: int
        @return: Surcharge time (hours).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_surcharge_time(h, idx, &v))
        return v

    # ====================================================================
    # Per-element statistics: subcatchments
    # ====================================================================

    def subcatch_precip(self, int idx) -> float:
        """Return the total precipitation on a subcatchment.

        Wraps C{swmm_stat_subcatch_precip}.

        @param idx: Zero-based subcatchment index.
        @type idx: int
        @return: Total precipitation volume (project depth x area units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_precip(h, idx, &v))
        return v

    def subcatch_runoff_vol(self, int idx) -> float:
        """Return the total runoff volume from a subcatchment.

        Wraps C{swmm_stat_subcatch_runoff_vol}.

        @param idx: Zero-based subcatchment index.
        @type idx: int
        @return: Total runoff volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_runoff_vol(h, idx, &v))
        return v

    def subcatch_max_runoff(self, int idx) -> float:
        """Return the maximum runoff rate from a subcatchment.

        Wraps C{swmm_stat_subcatch_max_runoff}.

        @param idx: Zero-based subcatchment index.
        @type idx: int
        @return: Maximum runoff rate (project flow units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_max_runoff(h, idx, &v))
        return v

    # ====================================================================
    # Cumulative totals (bulk array reads)
    # ====================================================================

    def node_max_depth_bulk(self) -> np.ndarray:
        """Return maximum depths for all nodes as a NumPy array.

        Wraps C{swmm_stat_node_max_depth_bulk}.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_node_max_depth_bulk(h, <double*>buf.data, n))
        return buf

    def link_max_flow_bulk(self) -> np.ndarray:
        """Return maximum flows for all links as a NumPy array.

        Wraps C{swmm_stat_link_max_flow_bulk}.

        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_link_max_flow_bulk(h, <double*>buf.data, n))
        return buf

    def subcatch_runoff_vol_bulk(self) -> np.ndarray:
        """Return total runoff volumes for all subcatchments as a NumPy array.

        Wraps C{swmm_stat_subcatch_runoff_vol_bulk}.

        @return: Array of shape C{(n_subcatchments,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_stat_subcatch_runoff_vol_bulk(h, <double*>buf.data, n))
        return buf
