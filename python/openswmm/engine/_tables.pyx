"""
Tables, curves, patterns (Pythonic v1 surface)
==============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Three collections live on the engine — time series, curves, and
patterns — and the C side stores time series and curves in a single
`tables` namespace. The Pythonic surface exposes:

* :class:`Tables` — the unified ``solver.tables`` collection. Indexable
  by ``int | str``. Items are :class:`TimeSeries` or :class:`Curve`
  wrappers depending on the type at construction time. Add new entries
  with ``tables.add_timeseries(id)`` or ``tables.add_curve(id, type)``.
* :class:`TimeSeries` — ``.points`` returns a structured numpy array
  with ``x`` as ``datetime64[s]``; supports point-list editing.
* :class:`Curve` — ``.points`` returns a ``float64`` ``(n, 2)`` array
  and ``Curve.lookup(x)`` runs the engine's interpolation.
* :class:`Patterns` — ``solver.patterns``, indexable; entries are
  :class:`Pattern` wrappers with a ``.factors`` numpy array.
"""

# cython: language_level=3

from typing import Tuple

import numpy as np
cimport numpy as np

from ._common cimport *
from ._dates import oadate_to_datetime, datetime_to_oadate
from ._enums import PatternType


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_table(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_table_index, swmm_table_count, "Table")


# A pattern collection has no swmm_pattern_index helper; we resolve by
# scanning the (small) pattern set.


# =============================================================================
# TimeSeries / Curve / shared point editor
# =============================================================================

cdef class _PointTable:
    """Common behaviour for TimeSeries + Curve — both store (x, y) points."""
    cdef object _solver
    cdef int _index
    cdef str _captured_id

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        cdef const char* raw = swmm_table_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""

    @property
    def id(self) -> str:
        cdef const char* raw = swmm_table_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def index(self) -> int:
        return self._index

    @property
    def solver(self):
        return self._solver

    def __len__(self) -> int:
        cdef int n = 0
        _check(swmm_table_get_point_count(_h(self._solver), self._index, &n))
        return n

    def add_point(self, double x, double y) -> None:
        _check(swmm_table_add_point(_h(self._solver), self._index, x, y))

    def clear(self) -> None:
        """Remove all points."""
        _check(swmm_table_clear(_h(self._solver), self._index))

    def lookup(self, double x) -> float:
        """Interpolate y at x using the underlying SWMM lookup."""
        cdef double v = 0.0
        _check(swmm_table_lookup(_h(self._solver), self._index, x, &v))
        return v

    def _raw_points(self):
        cdef int n = 0
        _check(swmm_table_get_point_count(_h(self._solver), self._index, &n))
        cdef np.ndarray[double, ndim=2] buf = np.empty((n, 2), dtype=np.float64)
        cdef double x = 0.0, y = 0.0
        for i in range(n):
            _check(swmm_table_get_point(_h(self._solver), self._index, i, &x, &y))
            buf[i, 0] = x
            buf[i, 1] = y
        return buf

    def __repr__(self) -> str:
        try:
            return f"<{type(self).__name__} id={self._captured_id!r} n_points={len(self)}>"
        except Exception:
            return f"<{type(self).__name__} index={self._index} (closed)>"


cdef class TimeSeries(_PointTable):
    """A SWMM time series — x is a SWMM DateTime double; we surface it as
    ``datetime64[s]``."""

    @property
    def points(self):
        """Structured numpy array with columns ``time: datetime64[s]`` and
        ``value: float64``."""
        raw = self._raw_points()
        n = raw.shape[0]
        dtype = np.dtype([("time", "datetime64[s]"), ("value", "float64")])
        out = np.empty(n, dtype=dtype)
        for i in range(n):
            dt = oadate_to_datetime(float(raw[i, 0])).replace(microsecond=0)
            out["time"][i] = np.datetime64(dt)
        out["value"][:] = raw[:, 1]
        return out

    def add(self, when, double value) -> None:
        """Append a ``(when, value)`` point. ``when`` is :class:`datetime` or
        a SWMM DateTime float."""
        from datetime import datetime
        cdef double x
        if isinstance(when, datetime):
            x = datetime_to_oadate(when)
        else:
            x = float(when)
        self.add_point(x, value)


cdef class Curve(_PointTable):
    """A SWMM curve — x and y are both ordinary floats."""

    @property
    def points(self):
        """``float64`` ``(n_points, 2)`` numpy array."""
        return self._raw_points()


# =============================================================================
# Tables collection (unified time-series + curves)
# =============================================================================

cdef class Tables:
    """``solver.tables`` — indexable collection of :class:`TimeSeries` and
    :class:`Curve` wrappers.

    The C side gives them a single shared id namespace, so this view
    indexes both. The returned wrapper class depends on how the entry
    was originally added (``add_timeseries`` vs ``add_curve``).
    """

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Container -----------------------------------------------

    def __len__(self) -> int:
        return swmm_table_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_table_count(_h(self._solver))
        for i in range(n):
            yield self._wrap(i)

    def __getitem__(self, key):
        cdef int i = _resolve_table(self._solver, key)
        return self._wrap(i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_table(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    def get_index(self, str table_id) -> int:
        cdef bytes b = table_id.encode('utf-8')
        cdef int i = swmm_table_index(_h(self._solver), b)
        if i < 0:
            raise KeyError(table_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_table_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Creation -----------------------------------------------

    def add_timeseries(self, str ts_id) -> TimeSeries:
        cdef bytes b = ts_id.encode('utf-8')
        _check(swmm_timeseries_add(_h(self._solver), b))
        self._solver._bump_generation()
        cdef int idx = swmm_table_index(_h(self._solver), b)
        return TimeSeries(self._solver, idx)

    def add_curve(self, str curve_id, int curve_type=0) -> Curve:
        """``curve_type`` is the legacy CurveType integer code; the C
        API accepts 0 for the default."""
        cdef bytes b = curve_id.encode('utf-8')
        _check(swmm_curve_add(_h(self._solver), b, curve_type))
        self._solver._bump_generation()
        cdef int idx = swmm_table_index(_h(self._solver), b)
        return Curve(self._solver, idx)

    # ---- Internal -----------------------------------------------

    def _wrap(self, int idx):
        # No type-discriminator entry-point on the C side today; we hand back
        # the generic _PointTable which behaves correctly for both. Callers
        # that want the typed view can construct ``TimeSeries(solver, idx)``
        # or ``Curve(solver, idx)`` explicitly.
        return _PointTable(self._solver, idx)

    def as_timeseries(self, key) -> TimeSeries:
        cdef int i = _resolve_table(self._solver, key)
        return TimeSeries(self._solver, i)

    def as_curve(self, key) -> Curve:
        cdef int i = _resolve_table(self._solver, key)
        return Curve(self._solver, i)

    def __repr__(self) -> str:
        try:
            return f"<Tables n={len(self)}>"
        except Exception:
            return "<Tables (engine closed)>"


# =============================================================================
# Patterns
# =============================================================================

cdef class Pattern:
    """One ``[PATTERNS]`` entry."""
    cdef object _solver
    cdef int _index

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index

    @property
    def index(self) -> int:
        return self._index

    @property
    def solver(self):
        return self._solver

    def set_factors(self, values, type=PatternType.HOURLY) -> None:
        """Replace the pattern factors. ``values`` must be a sequence with
        length 12 (monthly), 7 (daily), 24 (hourly), or 24 (weekend)."""
        cdef np.ndarray[double, ndim=1] arr = np.ascontiguousarray(
            values, dtype=np.float64)
        cdef int n = arr.shape[0]
        _check(swmm_pattern_set_factors(
            _h(self._solver), self._index, <const double*>arr.data, n))

    def __repr__(self) -> str:
        return f"<Pattern index={self._index}>"


cdef class Patterns:
    """``solver.patterns`` — index-keyed collection of :class:`Pattern`
    wrappers. The C API does not expose an id→index lookup, so string
    keys are not currently supported here."""

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return swmm_pattern_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_pattern_count(_h(self._solver))
        for i in range(n):
            yield Pattern(self._solver, i)

    def __getitem__(self, int idx):
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        return Pattern(self._solver, idx)

    def add(self, str pattern_id, type=PatternType.HOURLY) -> Pattern:
        cdef bytes b = pattern_id.encode('utf-8')
        _check(swmm_pattern_add(_h(self._solver), b, int(type)))
        self._solver._bump_generation()
        # Newly-added pattern is the last one.
        return Pattern(self._solver, swmm_pattern_count(_h(self._solver)) - 1)

    def __repr__(self) -> str:
        try:
            return f"<Patterns n={len(self)}>"
        except Exception:
            return "<Patterns (engine closed)>"
