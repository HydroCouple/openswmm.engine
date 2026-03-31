"""
Subcatchment Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._subcatchments`.

The :class:`Subcatchments` class provides access to subcatchment properties
during a simulation.
"""

from typing import Union

from ._solver import Solver


class Subcatchments:
    """Access subcatchment properties during a simulation.

    All per-element methods accept either an integer index or a string
    subcatchment ID.  When a string is passed it is resolved via
    :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Subcatchments

        with Solver("model.inp", "model.rpt", "model.out") as s:
            sc = Subcatchments(s)
            runoff = sc.get_runoff(0)      # by index
            runoff = sc.get_runoff("S1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer index.

        Args:
            idx: Integer index or string subcatchment ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    def count(self) -> int:
        """Return the number of subcatchments.

        Returns:
            Subcatchment count.
        """
        ...

    def get_index(self, sc_id: str) -> int:
        """Return the index of a subcatchment by ID.

        Args:
            sc_id: Subcatchment identifier.

        Returns:
            Index, or -1 if not found.
        """
        ...

    def get_runoff(self, idx: Union[int, str]) -> float:
        """Return the current runoff rate from a subcatchment.

        Args:
            idx: Subcatchment index (int) or subcatchment ID (str).

        Returns:
            Runoff rate (project flow units).
        """
        ...

    def get_groundwater(self, idx: Union[int, str]) -> float:
        """Return the groundwater outflow from a subcatchment.

        Args:
            idx: Subcatchment index (int) or subcatchment ID (str).

        Returns:
            Groundwater flow rate (project flow units).
        """
        ...

    def set_rainfall(self, idx: Union[int, str], rainfall: float) -> None:
        """Override rainfall on a subcatchment for the current timestep.

        Args:
            idx: Subcatchment index (int) or subcatchment ID (str).
            rainfall: Rainfall rate (project rainfall units).
                      Negative value reverts to gage-driven.
        """
        ...
