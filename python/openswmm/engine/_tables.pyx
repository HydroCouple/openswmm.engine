"""
Table / Curve / Time-Series Access
===================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Tables` class provides access to curves, time-series, and
pattern objects stored in the model.

.. code-block:: python

    from openswmm.engine import Solver, Tables

    with Solver("model.inp", "model.rpt", "model.out") as s:
        tables = Tables(s)
        print(f"Table count: {tables.count()}")
        y = tables.lookup("StorageCurve", 2.5)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Tables:
    """Access curve, time-series, and pattern data.

    All per-element methods accept either an integer index or a string
    table ID.  When a string is passed it is resolved via L{get_index}.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct a L{Tables} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent table operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve C{idx} to an integer table index.

        @param idx: Integer index or string table ID.
        @type idx: Union[int, str]
        @return: Integer table index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Table '{idx}' not found")
            return i
        return idx

    # ====================================================================
    # Time series / Curves -- identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of tables (curves + time-series) in the model.

        @return: Table count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_table_count(h)

    def get_index(self, str table_id) -> int:
        """Return the integer index of a table by its string ID.

        @param table_id: Table identifier string.
        @type table_id: str
        @return: Table index, or C{-1} if not found.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = table_id.encode('utf-8')
        return swmm_table_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a table by index.

        @param idx: Table index.
        @type idx: int
        @return: Table ID string, or empty string if C{idx} is invalid.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_table_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ====================================================================
    # Curves / Time series -- data points
    # ====================================================================

    def add_point(self, idx, double x, double y):
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
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_table_add_point(h, i, x, y))

    def get_point_count(self, idx) -> int:
        """Return the number of data points in a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @return: Point count.
        @rtype: int
        @raise EngineError: If the underlying C{swmm_table_get_point_count}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_table_get_point_count(h, i, &count))
        return count

    def get_point(self, idx, int pt_idx) -> tuple:
        """Return a single data point from a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @param pt_idx: Point index within the table.
        @type pt_idx: int
        @return: Tuple of C{(x, y)}.
        @rtype: tuple
        @raise EngineError: If the underlying C{swmm_table_get_point} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_table_get_point(h, i, pt_idx, &x, &y))
        return (x, y)

    def clear(self, idx):
        """Remove all data points from a table.

        @param idx: Table index (int) or table ID (str).
        @type idx: Union[int, str]
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_table_clear} call
            returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_table_clear(h, i))

    # ====================================================================
    # Lookup
    # ====================================================================

    def lookup(self, idx, double x) -> float:
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
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double y = 0.0
        _check(swmm_table_lookup(h, i, x, &y))
        return y

    # ====================================================================
    # Patterns
    # ====================================================================

    def pattern_count(self) -> int:
        """Return the number of time patterns in the model.

        @return: Pattern count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_pattern_count(h)
