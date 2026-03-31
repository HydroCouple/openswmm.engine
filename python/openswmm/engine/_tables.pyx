"""
Table / Curve / Time-Series Access
===================================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept either an integer index or a string
    table ID.  When a string is passed it is resolved via :meth:`get_index`.
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve *idx* to an integer table index.

        :param idx: Integer index or string table ID.
        :returns: Integer index.
        :raises KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Table '{idx}' not found")
            return i
        return idx

    # ------------------------------------------------------------------
    # Identity
    # ------------------------------------------------------------------

    def count(self) -> int:
        """Return the number of tables (curves + time-series) in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_table_count(h)

    def get_index(self, str table_id) -> int:
        """Return the integer index of a table by its string ID.

        :param table_id: Table identifier string.
        :returns: Table index, or -1 if not found.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = table_id.encode('utf-8')
        return swmm_table_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a table by index.

        :param idx: Table index.
        :returns: Table ID string.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_table_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ------------------------------------------------------------------
    # Data points
    # ------------------------------------------------------------------

    def add_point(self, idx, double x, double y):
        """Append a data point to a table.

        :param idx: Table index (int) or table ID (str).
        :param x: X value.
        :param y: Y value.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_table_add_point(h, i, x, y))

    def get_point_count(self, idx) -> int:
        """Return the number of data points in a table.

        :param idx: Table index (int) or table ID (str).
        :returns: Point count.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_table_get_point_count(h, i, &count))
        return count

    def get_point(self, idx, int pt_idx) -> tuple:
        """Return a single data point from a table.

        :param idx: Table index (int) or table ID (str).
        :param pt_idx: Point index within the table.
        :returns: Tuple of (x, y).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_table_get_point(h, i, pt_idx, &x, &y))
        return (x, y)

    def clear(self, idx):
        """Remove all data points from a table.

        :param idx: Table index (int) or table ID (str).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_table_clear(h, i))

    # ------------------------------------------------------------------
    # Lookup
    # ------------------------------------------------------------------

    def lookup(self, idx, double x) -> float:
        """Interpolate a Y value from a table for a given X.

        :param idx: Table index (int) or table ID (str).
        :param x: X value to look up.
        :returns: Interpolated Y value.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double y = 0.0
        _check(swmm_table_lookup(h, i, x, &y))
        return y

    # ------------------------------------------------------------------
    # Patterns
    # ------------------------------------------------------------------

    def pattern_count(self) -> int:
        """Return the number of time patterns in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_pattern_count(h)
