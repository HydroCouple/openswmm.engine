"""
Mass Balance and Continuity
===========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`MassBalance` class provides continuity error queries after a
simulation completes.

.. code-block:: python

    from openswmm.engine import Solver, MassBalance

    with Solver("model.inp") as s:
        while s.step():
            pass

    mb = MassBalance(s)
    print(f"Runoff error:  {mb.get_runoff_continuity_error():.4%}")
    print(f"Routing error: {mb.get_routing_continuity_error():.4%}")
"""

# cython: language_level=3

from ._common cimport *


class MassBalance:
    """Query continuity errors and cumulative flux totals.

    :param solver: An active :class:`~openswmm.engine.Solver` instance
        (typically after the simulation has ended).

    .. code-block:: python

        mb = MassBalance(solver)
        err = mb.get_runoff_continuity_error()
    """

    def __init__(self, solver):
        self._solver = solver

    def get_runoff_continuity_error(self) -> float:
        """Return the runoff continuity error.

        :returns: Continuity error as a fraction (e.g., 0.001 = 0.1%).
        :rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_continuity_error(h, &v))
        return v

    def get_routing_continuity_error(self) -> float:
        """Return the routing continuity error.

        :returns: Continuity error as a fraction.
        :rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_continuity_error(h, &v))
        return v

    def get_quality_continuity_error(self, int pollutant_idx) -> float:
        """Return the quality continuity error for a pollutant.

        :param pollutant_idx: Pollutant index.
        :returns: Continuity error as a fraction.
        :rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_quality_continuity_error(h, pollutant_idx, &v))
        return v

    def get_runoff_total(self, int component) -> float:
        """Return a cumulative runoff total.

        :param component: SWMM_RunoffTotal enum code (0-6).
        :returns: Cumulative volume (project volume units).
        :rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_runoff_total(h, component, &v))
        return v

    def get_routing_total(self, int component) -> float:
        """Return a cumulative routing total.

        :param component: SWMM_RoutingTotal enum code (0-10).
        :returns: Cumulative volume (project volume units).
        :rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_get_routing_total(h, component, &v))
        return v
