"""
Rain Gage Access
================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Gages` class provides access to rain gage rainfall during a
simulation.

.. code-block:: python

    from openswmm.engine import Solver, Gages

    with Solver("model.inp", "model.rpt", "model.out") as s:
        gages = Gages(s)
        while s.step():
            rain = gages.get_rainfall("RainGage")    # by name
            gages.set_rainfall(0, 25.4)               # or by index
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Gages:
    """Access and override rain gage rainfall during a simulation.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept either an integer index or a string gage
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    .. code-block:: python

        gages = Gages(solver)
        rain = gages.get_rainfall(0)           # by index
        rain = gages.get_rainfall("RainGage")  # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve *idx* to an integer index.

        :param idx: Integer index or string gage ID.
        :returns: Integer index.
        :raises KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Gage '{idx}' not found")
            return i
        return idx

    def count(self) -> int:
        """Return the number of rain gages.

        :returns: Gage count.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_gage_count(h)

    def get_index(self, str gage_id) -> int:
        """Return the index of a rain gage by ID.

        :param gage_id: Gage identifier.
        :returns: Index, or -1 if not found.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = gage_id.encode('utf-8')
        return swmm_gage_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the ID string of a rain gage by index.

        :param idx: Gage index.
        :returns: Gage identifier, or empty string if not found.
        :rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* s = swmm_gage_id(h, idx)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def add(self, str gage_id) -> int:
        """Add a rain gage to the model (OPENED-state editing).

        Wraps ``swmm_gage_add``. Valid in ``BUILDING`` or ``OPENED``
        state. For from-scratch construction without an .inp file, use
        :class:`ModelBuilder.add_gage`.

        :param gage_id: Unique gage identifier.
        :returns: Error code (0 on success).
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = gage_id.encode('utf-8')
        return swmm_gage_add(h, b)

    def get_rainfall(self, idx) -> float:
        """Return the current rainfall rate at a gage.

        :param idx: Gage index (int) or gage ID (str).
        :returns: Rainfall rate (project rainfall units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_gage_get_rainfall(h, i, &v))
        return v

    def set_rainfall(self, idx, double rainfall):
        """Override rainfall at a gage for the current timestep.

        Affects all subcatchments that use this gage. Applied for one
        timestep only -- call again each step to sustain.

        :param idx: Gage index (int) or gage ID (str).
        :param rainfall: Rainfall rate (project rainfall units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_gage_set_rainfall(h, i, rainfall))

    def set_rain_type(self, idx, int type):
        """Set the rain type for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :param type: Rain type code.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_gage_set_rain_type(h, i, type))

    def set_rain_interval(self, idx, double seconds):
        """Set the rain recording interval for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :param seconds: Recording interval in seconds.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_gage_set_rain_interval(h, i, seconds))

    def set_data_source(self, idx, int source):
        """Set the data source type for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :param source: Data source code.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_gage_set_data_source(h, i, source))

    def set_timeseries(self, idx, str ts_id):
        """Set the timeseries data source for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :param ts_id: Timeseries identifier.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = ts_id.encode('utf-8')
        _check(swmm_gage_set_timeseries(h, i, b))

    def set_filename(self, idx, str path, str station_id):
        """Set the external rainfall file and station for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :param path: Path to the rainfall data file.
        :param station_id: Station identifier within the file.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_path = path.encode('utf-8')
        cdef bytes b_station = station_id.encode('utf-8')
        _check(swmm_gage_set_filename(h, i, b_path, b_station))

    def get_rain_type(self, idx) -> int:
        """Return the rain type code for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :returns: Rain type code.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_gage_get_rain_type(h, i, &v))
        return v

    def get_data_source(self, idx) -> int:
        """Return the data source code for a gage.

        :param idx: Gage index (int) or gage ID (str).
        :returns: Data source code.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_gage_get_data_source(h, i, &v))
        return v

    def get_rainfall_bulk(self) -> np.ndarray:
        """Return rainfall for all gages as a NumPy array.

        :returns: 1-D array of rainfall values, one per gage.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_gage_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_gage_get_rainfall_bulk(h, <double*>buf.data, n))
        return buf
