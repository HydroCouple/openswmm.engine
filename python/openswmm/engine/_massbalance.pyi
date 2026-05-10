"""
Mass Balance and Continuity
===========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._massbalance`.

The :class:`MassBalance` class provides continuity error queries after a
simulation completes.
"""

from ._solver import Solver


class MassBalance:
    """Query continuity errors and cumulative flux totals.

    All methods operate on the engine handle held by the L{Solver}
    passed at construction time.

    Example::

        from openswmm.engine import Solver, MassBalance

        with Solver("model.inp") as s:
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                pass

        mb = MassBalance(s)
        print(f"Runoff error:  {mb.get_runoff_continuity_error():.4%}")
        print(f"Routing error: {mb.get_routing_continuity_error():.4%}")

    @ivar _solver: The L{Solver} instance providing the engine handle.
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{MassBalance} bound to a solver.

        @param solver: An active L{Solver} instance (typically after the
            simulation has ended).
        @type solver: Solver
        """
        ...

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
        ...

    def get_routing_continuity_error(self) -> float:
        """Return the routing continuity error.

        Wraps C{swmm_get_routing_continuity_error}.

        @return: Continuity error as a fraction.
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def get_quality_continuity_error(self, pollutant_idx: int) -> float:
        """Return the quality continuity error for a pollutant.

        Wraps C{swmm_get_quality_continuity_error}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Continuity error as a fraction.
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    # ====================================================================
    # Per-domain breakdowns (runoff / flow / quality)
    # ====================================================================

    def get_runoff_total(self, component: int) -> float:
        """Return a cumulative runoff total.

        Wraps C{swmm_get_runoff_total}.

        @param component: C{SWMM_RunoffTotal} enum code (C{0}-C{6}).
        @type component: int
        @return: Cumulative volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        @see: L{openswmm.engine.RunoffTotal}
        """
        ...

    def get_routing_total(self, component: int) -> float:
        """Return a cumulative routing total.

        Wraps C{swmm_get_routing_total}.

        @param component: C{SWMM_RoutingTotal} enum code (C{0}-C{10}).
        @type component: int
        @return: Cumulative volume (project volume units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        @see: L{openswmm.engine.RoutingTotal}
        """
        ...

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
        ...

    def get_max_courant(self) -> float:
        """Return the maximum Courant number observed during simulation.

        Wraps C{swmm_get_max_courant}.

        @return: Maximum Courant number (dimensionless).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def get_quality_seep_loss(self, pollutant_idx: int) -> float:
        """Return quality mass lost to seepage for a pollutant.

        Wraps C{swmm_get_quality_seep_loss}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Cumulative mass lost to seepage (project mass units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def get_quality_evap_loss(self, pollutant_idx: int) -> float:
        """Return quality mass lost to evaporation for a pollutant.

        Wraps C{swmm_get_quality_evap_loss}.

        @param pollutant_idx: Zero-based pollutant index.
        @type pollutant_idx: int
        @return: Cumulative mass lost to evaporation (project mass units).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        ...
