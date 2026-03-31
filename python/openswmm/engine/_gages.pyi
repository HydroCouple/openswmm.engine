"""
Rain Gage Access
================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._gages`.

The :class:`Gages` class provides access to rain gage rainfall during a
simulation.
"""

from typing import Union

from ._solver import Solver


class Gages:
    """Access and override rain gage rainfall during a simulation.

    All per-element methods accept either an integer index or a string gage
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Gages

        with Solver("model.inp", "model.rpt", "model.out") as s:
            gages = Gages(s)
            rain = gages.get_rainfall(0)           # by index
            rain = gages.get_rainfall("RainGage")  # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer index.

        Args:
            idx: Integer index or string gage ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    def count(self) -> int:
        """Return the number of rain gages.

        Returns:
            Gage count.
        """
        ...

    def get_index(self, gage_id: str) -> int:
        """Return the index of a rain gage by ID.

        Args:
            gage_id: Gage identifier.

        Returns:
            Index, or -1 if not found.
        """
        ...

    def get_rainfall(self, idx: Union[int, str]) -> float:
        """Return the current rainfall rate at a gage.

        Args:
            idx: Gage index (int) or gage ID (str).

        Returns:
            Rainfall rate (project rainfall units).
        """
        ...

    def set_rainfall(self, idx: Union[int, str], rainfall: float) -> None:
        """Override rainfall at a gage for the current timestep.

        Affects all subcatchments that use this gage. Applied for one
        timestep only -- call again each step to sustain.

        Args:
            idx: Gage index (int) or gage ID (str).
            rainfall: Rainfall rate (project rainfall units).
        """
        ...
