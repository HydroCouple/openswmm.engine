"""
Mass Balance and Continuity
===========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._massbalance`.

The :class:`MassBalance` class provides continuity error queries after a
simulation completes.
"""

from ._solver import Solver


class MassBalance:
    """Query continuity errors and cumulative flux totals.

    All methods operate on the engine handle held by the :class:`Solver`
    passed at construction time.

    Args:
        solver: An active :class:`Solver` instance (typically after the
                simulation has ended).

    Example::

        from openswmm.engine import Solver, MassBalance

        with Solver("model.inp") as s:
            while s.step():
                pass

        mb = MassBalance(s)
        print(f"Runoff error:  {mb.get_runoff_continuity_error():.4%}")
        print(f"Routing error: {mb.get_routing_continuity_error():.4%}")
    """

    def __init__(self, solver: Solver) -> None: ...

    def get_runoff_continuity_error(self) -> float:
        """Return the runoff continuity error.

        Returns:
            Continuity error as a fraction (e.g., 0.001 = 0.1%).
        """
        ...

    def get_routing_continuity_error(self) -> float:
        """Return the routing continuity error.

        Returns:
            Continuity error as a fraction.
        """
        ...
