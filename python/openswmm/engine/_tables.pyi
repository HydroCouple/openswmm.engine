"""
Table / Curve / Time-Series Access
===================================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._tables`.

The :class:`Tables` class provides access to curves, time-series, and
pattern objects stored in the model.
"""

from typing import Union

from ._solver import Solver


class Tables:
    """Access curve, time-series, and pattern data.

    All per-element methods accept either an integer index or a string
    table ID. When a string is passed it is resolved via :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Tables

        with Solver("model.inp", "model.rpt", "model.out") as s:
            tables = Tables(s)
            print(f"Table count: {tables.count()}")
            y = tables.lookup("StorageCurve", 2.5)
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer table index.

        Args:
            idx: Integer index or string table ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    # ------------------------------------------------------------------
    # Identity
    # ------------------------------------------------------------------

    def count(self) -> int:
        """Return the number of tables (curves + time-series) in the model.

        Returns:
            Table count.
        """
        ...

    def get_index(self, table_id: str) -> int:
        """Return the integer index of a table by its string ID.

        Args:
            table_id: Table identifier string.

        Returns:
            Table index, or -1 if not found.
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a table by index.

        Args:
            idx: Table index.

        Returns:
            Table ID string.
        """
        ...

    # ------------------------------------------------------------------
    # Data points
    # ------------------------------------------------------------------

    def add_point(self, idx: Union[int, str], x: float, y: float) -> None:
        """Append a data point to a table.

        Args:
            idx: Table index (int) or table ID (str).
            x: X value.
            y: Y value.
        """
        ...

    def get_point_count(self, idx: Union[int, str]) -> int:
        """Return the number of data points in a table.

        Args:
            idx: Table index (int) or table ID (str).

        Returns:
            Point count.
        """
        ...

    def get_point(self, idx: Union[int, str], pt_idx: int) -> tuple[float, float]:
        """Return a single data point from a table.

        Args:
            idx: Table index (int) or table ID (str).
            pt_idx: Point index within the table.

        Returns:
            Tuple of (x, y).
        """
        ...

    def clear(self, idx: Union[int, str]) -> None:
        """Remove all data points from a table.

        Args:
            idx: Table index (int) or table ID (str).
        """
        ...

    # ------------------------------------------------------------------
    # Lookup
    # ------------------------------------------------------------------

    def lookup(self, idx: Union[int, str], x: float) -> float:
        """Interpolate a Y value from a table for a given X.

        Args:
            idx: Table index (int) or table ID (str).
            x: X value to look up.

        Returns:
            Interpolated Y value.
        """
        ...

    # ------------------------------------------------------------------
    # Patterns
    # ------------------------------------------------------------------

    def pattern_count(self) -> int:
        """Return the number of time patterns in the model.

        Returns:
            Pattern count.
        """
        ...
