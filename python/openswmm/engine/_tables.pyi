"""
Table / Curve / Time-Series Access
===================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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
    table ID. When a string is passed it is resolved via L{get_index}.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Tables

        with Solver("model.inp", "model.rpt", "model.out") as s:
            tables = Tables(s)
            print(f"Table count: {tables.count()}")
            y = tables.lookup("StorageCurve", 2.5)

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{Tables} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent table operations.
        @type solver: L{Solver}
        """
        ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer table index.

        @param idx: Integer index or string table ID.
        @type idx: Union[int, str]
        @return: Integer table index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        ...

    # ====================================================================
    # Time series / Curves -- identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of tables (curves + time-series) in the model.

        @return: Table count.
        @rtype: int
        """
        ...

    def get_index(self, table_id: str) -> int:
        """Return the integer index of a table by its string ID.

        @param table_id: Table identifier string.
        @type table_id: str
        @return: Table index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a table by index.

        @param idx: Table index.
        @type idx: int
        @return: Table ID string, or empty string if C{idx} is invalid.
        @rtype: str
        """
        ...

    # ====================================================================
    # Curves / Time series -- data points
    # ====================================================================

    def add_point(self, idx: Union[int, str], x: float, y: float) -> None:
        """Append a data point to a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @param x: X value.
        @type x: float
        @param y: Y value.
        @type y: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_table_add_point} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_point_count(self, idx: Union[int, str]) -> int:
        """Return the number of data points in a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @return: Point count.
        @rtype: int
        @raise EngineError: If the underlying C{swmm_table_get_point_count}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_point(self, idx: Union[int, str], pt_idx: int) -> tuple[float, float]:
        """Return a single data point from a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @param pt_idx: Point index within the table.
        @type pt_idx: int
        @return: Tuple of C{(x, y)}.
        @rtype: tuple[float, float]
        @raise EngineError: If the underlying C{swmm_table_get_point} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def clear(self, idx: Union[int, str]) -> None:
        """Remove all data points from a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_table_clear} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    # ====================================================================
    # Lookup
    # ====================================================================

    def lookup(self, idx: Union[int, str], x: float) -> float:
        """Interpolate a Y value from a table for a given X.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @param x: X value to look up.
        @type x: float
        @return: Interpolated Y value.
        @rtype: float
        @raise EngineError: If the underlying C{swmm_table_lookup} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    # ====================================================================
    # Patterns
    # ====================================================================

    def pattern_count(self) -> int:
        """Return the number of time patterns in the model.

        @return: Pattern count.
        @rtype: int
        """
        ...
