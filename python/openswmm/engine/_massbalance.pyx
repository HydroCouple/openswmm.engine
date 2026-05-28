"""
Mass balance & continuity (Pythonic v1 surface)
===============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`MassBalance` view exposes continuity errors, flux totals,
and routing diagnostics for a completed simulation. It is reached via
``solver.mass_balance`` (the lazy property on :class:`Solver`).

.. code-block:: python

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass
        mb = s.mass_balance
        print(mb.runoff_continuity_error)
        print(mb.routing_continuity_error)
        print(mb.routing_total(RoutingTotal.OUTFLOW))
        print(mb.routing_diagnostics.max_courant)
"""

# cython: language_level=3

from ._common cimport *
from ._enums import RoutingTotal, RunoffTotal
from ._report import RoutingDiagnostics


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


class MassBalance:
    """Mass-balance accessors reached via ``solver.mass_balance``.

    Pure-Python facade — no cdef state. Per-call C lookups so the
    values always reflect the current engine state.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Continuity errors (properties — no arguments)
    # ------------------------------------------------------------------

    @property
    def runoff_continuity_error(self) -> float:
        """Runoff continuity error (%)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_continuity_error(h, &v))
        return v

    @property
    def routing_continuity_error(self) -> float:
        """Flow routing continuity error (%)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_continuity_error(h, &v))
        return v

    def quality_continuity_error(self, pollutant) -> float:
        """Quality continuity error (%) for ``pollutant`` (id or index)."""
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_continuity_error(h, p, &v))
        return v

    # ------------------------------------------------------------------
    # Flux totals (enum-typed components)
    # ------------------------------------------------------------------

    def runoff_total(self, component) -> float:
        """Cumulative runoff volume for ``component`` (:class:`RunoffTotal`)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_total(h, int(component), &v))
        return v

    def routing_total(self, component) -> float:
        """Cumulative routing volume for ``component`` (:class:`RoutingTotal`)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_total(h, int(component), &v))
        return v

    # ------------------------------------------------------------------
    # Routing diagnostics
    # ------------------------------------------------------------------

    @property
    def routing_diagnostics(self) -> RoutingDiagnostics:
        """Routing-solver time-step diagnostics as a
        :class:`RoutingDiagnostics` dataclass."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double avg_s = 0, min_s = 0, max_s = 0
        cdef double pct_nc = 0, avg_it = 0, mx_co = 0
        cdef int n_steps = 0
        _check(swmm_get_routing_stats(h, &avg_s, &min_s, &max_s,
                                       &n_steps, &pct_nc, &avg_it, &mx_co))
        return RoutingDiagnostics(
            avg_time_step=avg_s,
            min_time_step=min_s,
            max_time_step=max_s,
            n_steps=n_steps,
            pct_not_converged=pct_nc,
            n_steps_not_converged=int(round(n_steps * pct_nc / 100.0)),
            avg_iterations=avg_it,
            max_courant=mx_co,
        )

    @property
    def max_courant(self) -> float:
        """Maximum Courant number observed during the simulation."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_max_courant(h, &v))
        return v

    # ------------------------------------------------------------------
    # Quality mass losses
    # ------------------------------------------------------------------

    def quality_seep_loss(self, pollutant) -> float:
        """Cumulative mass of ``pollutant`` lost to seepage."""
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_seep_loss(h, p, &v))
        return v

    def quality_evap_loss(self, pollutant) -> float:
        """Cumulative mass of ``pollutant`` lost to evaporation."""
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_evap_loss(h, p, &v))
        return v

    def __repr__(self) -> str:
        try:
            return (f"<MassBalance runoff_err={self.runoff_continuity_error:.3f}% "
                    f"routing_err={self.routing_continuity_error:.3f}%>")
        except Exception:
            return "<MassBalance (engine not ENDED)>"
