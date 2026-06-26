"""
Rain gage access (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Gages` collection and :class:`Gage` wrapper expose rain
gages with the same shape as :mod:`openswmm.engine._nodes`.

.. code-block:: python

    from openswmm.engine import Solver, GageDataSource, GageRainType

    with Solver("model.inp") as s:
        g = s.gages["RG1"]
        print(g.rain_type, g.data_source)
        g.rainfall = 25.4

        # Bulk read of all rainfalls.
        arr = s.gages.rainfalls           # np.ndarray
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *
from ._enums import GageDataSource, GageRainType
from ._exceptions import ElementNotFoundError, StaleObjectError


# =============================================================================
# Helpers
# =============================================================================

cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_gage(solver, object key) except -1:
    return _resolve_index(_h(solver), key, swmm_gage_index, swmm_gage_count, "Gage")


cdef inline void _check_fresh(gage) except *:
    if gage._gen != gage._solver.generation:
        raise StaleObjectError(
            f"Gage wrapper (id={gage._captured_id!r}, index={gage._index}) is stale; "
            "look it up again from solver.gages."
        )


# =============================================================================
# Gage wrapper
# =============================================================================

cdef class Gage:
    """A single rain gage."""

    cdef readonly object _solver
    cdef readonly int _index
    cdef readonly long long _gen
    cdef readonly str _captured_id

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        self._gen = solver.generation
        cdef const char* raw = swmm_gage_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""

    # ---- Identity ---------------------------------------------------

    @property
    def id(self) -> str:
        _check_fresh(self)
        cdef const char* raw = swmm_gage_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def index(self) -> int:
        _check_fresh(self)
        return self._index

    @property
    def solver(self):
        return self._solver

    # ---- Configuration ---------------------------------------------

    @property
    def rain_type(self):
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_gage_get_rain_type(_h(self._solver), self._index, &v))
        return GageRainType(v)

    @rain_type.setter
    def rain_type(self, value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_rain_type(
            _h(self._solver), self._index, int(value)))

    @property
    def data_source(self):
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_gage_get_data_source(_h(self._solver), self._index, &v))
        return GageDataSource(v)

    @data_source.setter
    def data_source(self, value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_data_source(
            _h(self._solver), self._index, int(value)))

    @property
    def scale_factor(self) -> float:
        """Rainfall scaling factor (dimensionless, > 0; 1.0 = no scaling).

        May be mutated at any time, including while the simulation is running,
        to support parameter sweeps. The new value takes effect on the next
        timestep. Raises if assigned a non-positive value.
        """
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_gage_get_scale_factor(_h(self._solver), self._index, &v))
        return v

    @scale_factor.setter
    def scale_factor(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_scale_factor(_h(self._solver), self._index, value))

    @property
    def snow_factor(self) -> float:
        """Snow-catch deficiency correction factor (SCF; 1.0 = no correction).

        This is the legacy [RAINGAGES] SCF column, distinct from
        :attr:`scale_factor`. Raises if assigned a non-positive value.
        """
        _check_fresh(self)
        cdef double v = 1.0
        _check(swmm_gage_get_snow_factor(_h(self._solver), self._index, &v))
        return v

    @snow_factor.setter
    def snow_factor(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_snow_factor(_h(self._solver), self._index, value))

    @property
    def rain_interval(self) -> float:
        """Rain recording interval in seconds."""
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_gage_get_rain_interval(_h(self._solver), self._index, &v))
        return v

    @rain_interval.setter
    def rain_interval(self, value) -> None:
        self.set_rain_interval(value)

    @property
    def rain_units(self) -> int:
        """Rain-depth units declared for a file source: 0 = IN, 1 = MM."""
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_gage_get_rain_units(_h(self._solver), self._index, &v))
        return v

    @rain_units.setter
    def rain_units(self, int value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_rain_units(_h(self._solver), self._index, value))

    @property
    def timeseries(self) -> str:
        """Assigned time-series id (empty when the source is not a series)."""
        _check_fresh(self)
        cdef char buf[256]
        _check(swmm_gage_get_timeseries(_h(self._solver), self._index, buf, 256))
        return buf.decode('utf-8')

    @property
    def station_id(self) -> str:
        """Station id for a file source (empty when unset)."""
        _check_fresh(self)
        cdef char buf[256]
        _check(swmm_gage_get_station_id(_h(self._solver), self._index, buf, 256))
        return buf.decode('utf-8')

    @station_id.setter
    def station_id(self, str value) -> None:
        _check_fresh(self)
        cdef bytes b = value.encode('utf-8')
        _check(swmm_gage_set_station_id(_h(self._solver), self._index, b))

    def set_rain_interval(self, seconds) -> None:
        """Set the rain-interval duration. Accepts a number of seconds
        or a :class:`datetime.timedelta`."""
        from datetime import timedelta
        _check_fresh(self)
        cdef double s
        if isinstance(seconds, timedelta):
            s = seconds.total_seconds()
        else:
            s = float(seconds)
        _check(swmm_gage_set_rain_interval(_h(self._solver), self._index, s))

    def set_timeseries(self, ts_id: str) -> None:
        """Configure the gage to read from a named time series."""
        _check_fresh(self)
        cdef bytes b = ts_id.encode('utf-8')
        _check(swmm_gage_set_timeseries(_h(self._solver), self._index, b))

    def set_file(self, path: str, station_id: str) -> None:
        """Configure the gage to read from an external file."""
        _check_fresh(self)
        cdef bytes b_path = path.encode('utf-8')
        cdef bytes b_id = station_id.encode('utf-8')
        _check(swmm_gage_set_filename(
            _h(self._solver), self._index, b_path, b_id))

    # ---- Runtime state ---------------------------------------------

    @property
    def rainfall(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_gage_get_rainfall(_h(self._solver), self._index, &v))
        return v

    @rainfall.setter
    def rainfall(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_gage_set_rainfall(_h(self._solver), self._index, value))

    # ---- Equality / repr -------------------------------------------

    def __eq__(self, other):
        if not isinstance(other, Gage):
            return NotImplemented
        return (self._solver is other._solver
                and self._index == other._index)

    def __hash__(self):
        return hash((id(self._solver), self._index))

    def __repr__(self) -> str:
        try:
            return f"<Gage id={self._captured_id!r} index={self._index}>"
        except Exception:
            return f"<Gage index={self._index} (stale or closed)>"


# =============================================================================
# Gages collection
# =============================================================================

cdef class Gages:
    """Indexable, iterable collection of :class:`Gage` wrappers."""

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Container protocol ----------------------------------------

    def __len__(self) -> int:
        return swmm_gage_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_gage_count(_h(self._solver))
        for i in range(n):
            yield Gage(self._solver, i)

    def __getitem__(self, key) -> Gage:
        cdef int i = _resolve_gage(self._solver, key)
        return Gage(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_gage(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    # ---- Identity lookups -----------------------------------------

    def get_index(self, str gage_id) -> int:
        cdef bytes b = gage_id.encode('utf-8')
        cdef int i = swmm_gage_index(_h(self._solver), b)
        if i < 0:
            raise ElementNotFoundError(gage_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_gage_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Editing -------------------------------------------------

    def add(self, str gage_id) -> Gage:
        cdef bytes b = gage_id.encode('utf-8')
        _check(swmm_gage_add(_h(self._solver), b))
        self._solver._bump_generation()
        cdef int new_idx = swmm_gage_index(_h(self._solver), b)
        return Gage(self._solver, new_idx)

    def rename(self, key, str new_id) -> None:
        cdef int i = _resolve_gage(self._solver, key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_gage_rename(_h(self._solver), i, b))
        self._solver._bump_generation()

    # ---- Bulk -----------------------------------------------------

    @property
    def rainfalls(self):
        """All rainfalls as a 1-D ``float64`` array."""
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_gage_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_gage_get_rainfall_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def ids(self):
        return np.asarray(
            [self.get_id(i) for i in range(len(self))], dtype=object)

    def __repr__(self) -> str:
        try:
            return f"<Gages n={len(self)}>"
        except Exception:
            return "<Gages (engine closed)>"
