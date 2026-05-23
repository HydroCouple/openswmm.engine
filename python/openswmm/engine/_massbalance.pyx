"""
Mass Balance and Continuity
===========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`MassBalance` class provides continuity error queries after a
simulation completes.

.. code-block:: python

    from openswmm.engine import Solver, MassBalance

    with Solver("model.inp") as s:
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass

    mb = MassBalance(s)
    print(f"Runoff error:  {mb.get_runoff_continuity_error():.4%}")
    print(f"Routing error: {mb.get_routing_continuity_error():.4%}")
"""

# cython: language_level=3

from ._common cimport *


class MassBalance:
    """Query continuity errors and cumulative flux totals.

    All methods operate on the engine handle held by the L{Solver}
    passed at construction time.

    Example::

        mb = MassBalance(solver)
        err = mb.get_runoff_continuity_error()

    @ivar _solver: The L{Solver} instance providing the engine handle.
    """

    def __init__(self, solver):
        """Construct a L{MassBalance} bound to a solver.

        @param solver: An active L{Solver} instance (typically after the
            simulation has ended).
        @type solver: Solver
        """
        self._solver = solver

    # ====================================================================
    # Continuity error queries
    # ====================================================================

    def get_runoff_continuity_error(self) -> float:
        """Return the runoff continuity error.

        Wraps C{swmm_get_runoff_continuity_error}.

        @return: Continuity error as a fraction (e.g., C{0.001} = 0.1%).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_continuity_error(h, &v))
        return v

    def get_routing_continuity_error(self) -> float:
        """Return the routing continuity error.

        Wraps C{swmm_get_routing_continuity_error}.

        @return: Continuity error as a fraction.
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_continuity_error(h, &v))
        return v

    def get_quality_continuity_error(self, int pollutant_idx) -> float:
        """Return the quality continuity error for a pollutant.

        Wraps C{swmm_get_quality_continuity_error}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Continuity error as a fraction.
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_continuity_error(h, pollutant_idx, &v))
        return v

    # ====================================================================
    # Per-domain breakdowns (runoff / flow / quality)
    # ====================================================================

    def get_runoff_total(self, int component) -> float:
        """Return a cumulative runoff total.

        Wraps C{swmm_get_runoff_total}.

        @param component: C{SWMM_RunoffTotal} enum code (C{0}-C{6}).
        @type component: int
        @return: Cumulative volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        @see: L{openswmm.engine.RunoffTotal}
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_total(h, component, &v))
        return v

    def get_routing_total(self, int component) -> float:
        """Return a cumulative routing total.

        Wraps C{swmm_get_routing_total}.

        @param component: C{SWMM_RoutingTotal} enum code (C{0}-C{10}).
        @type component: int
        @return: Cumulative volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        @see: L{openswmm.engine.RoutingTotal}
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_total(h, component, &v))
        return v

    # ------------------------------------------------------------------
    # Routing diagnostics (combined, single C call)
    # ------------------------------------------------------------------

    def get_routing_stats(self) -> dict:
        """Return combined routing statistics in a single call.

        Wraps C{swmm_get_routing_stats}. Provides time-step diagnostics
        for the routing solver.

        @return: Dictionary with keys C{avg_step}, C{min_step},
            C{max_step}, C{n_steps}, C{pct_non_converged},
            C{avg_iterations}, C{max_courant}.
        @rtype: dict
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double avg_s = 0, min_s = 0, max_s = 0
        cdef double pct_nc = 0, avg_it = 0, mx_co = 0
        cdef int n_steps = 0
        _check(swmm_get_routing_stats(h, &avg_s, &min_s, &max_s,
                                       &n_steps, &pct_nc, &avg_it, &mx_co))
        return {
            "avg_step": avg_s,
            "min_step": min_s,
            "max_step": max_s,
            "n_steps": n_steps,
            "pct_non_converged": pct_nc,
            "avg_iterations": avg_it,
            "max_courant": mx_co,
        }

    def get_max_courant(self) -> float:
        """Return the maximum Courant number observed during simulation.

        Wraps C{swmm_get_max_courant}.

        @return: Maximum Courant number (dimensionless).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_max_courant(h, &v))
        return v

    # ------------------------------------------------------------------
    # Quality mass losses (per-pollutant)
    # ------------------------------------------------------------------

    def get_quality_seep_loss(self, int pollutant_idx) -> float:
        """Return quality mass lost to seepage for a pollutant.

        Wraps C{swmm_get_quality_seep_loss}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Cumulative mass lost to seepage (project mass units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_seep_loss(h, pollutant_idx, &v))
        return v

    def get_quality_evap_loss(self, int pollutant_idx) -> float:
        """Return quality mass lost to evaporation for a pollutant.

        Wraps C{swmm_get_quality_evap_loss}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Cumulative mass lost to evaporation (project mass units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_evap_loss(h, pollutant_idx, &v))
        return v
