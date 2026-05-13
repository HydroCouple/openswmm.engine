"""
Rain Gage Access
================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._gages`.

The :class:`Gages` class provides access to rain gage rainfall during a
simulation.
"""

from typing import Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Gages:
    """Access and override rain gage rainfall during a simulation.

    All per-element methods accept either an integer index or a string gage
    ID.  When a string is passed it is resolved via L{get_index}.

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Gages

        with Solver("model.inp", "model.rpt", "model.out") as s:
            gages = Gages(s)
            rain = gages.get_rainfall(0)           # by index
            rain = gages.get_rainfall("RainGage")  # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string gage ID.
        @type idx: Union[int, str]
        @return: Integer index.
        @rtype: int
        @raise KeyError: If a string ID is not found.
        """
        ...

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of rain gages in the model.

        @return: Gage count.
        @rtype: int
        """
        ...

    def get_index(self, gage_id: str) -> int:
        """Return the integer index of a rain gage by its string ID.

        @param gage_id: Gage identifier.
        @type gage_id: str
        @return: Gage index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a rain gage by index.

        @param idx: Gage index.
        @type idx: int
        @return: Gage identifier, or empty string if not found.
        @rtype: str
        """
        ...

    def add(self, gage_id: str) -> int:
        """Add a rain gage to the model (OPENED-state editing).

        Wraps C{swmm_gage_add}. Valid in C{BUILDING} or C{OPENED} state.

        @param gage_id: Unique gage identifier.
        @type gage_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # ====================================================================
    # Rainfall get/set
    # ====================================================================

    def get_rainfall(self, idx: Union[int, str]) -> float:
        """Return the current rainfall rate at a gage.

        @param idx: Gage index (int) or gage ID (str). Strings are
            resolved via L{get_index}.
        @type idx: Union[int, str]
        @return: Rainfall rate (project rainfall units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def set_rainfall(self, idx: Union[int, str], rainfall: float) -> None:
        """Override rainfall at a gage for the current timestep.

        Affects all subcatchments that use this gage. Applied for one
        timestep only -- call again each step to sustain.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param rainfall: Rainfall rate (project rainfall units).
        @type rainfall: float
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def get_rainfall_bulk(self) -> npt.NDArray[np.float64]:
        """Return rainfall for all gages as a NumPy array.

        @return: 1-D array of rainfall values, one per gage. Shape
            C{(n_gages,)}, dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    # ====================================================================
    # Gage configuration (BUILDING / OPENED)
    # ====================================================================

    def set_rain_type(self, idx: Union[int, str], type: int) -> None:
        """Set the rain type for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param type: Rain type code.
        @type type: int
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def set_rain_interval(self, idx: Union[int, str], seconds: float) -> None:
        """Set the rain recording interval for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param seconds: Recording interval in seconds.
        @type seconds: float
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def set_data_source(self, idx: Union[int, str], source: int) -> None:
        """Set the data source type for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param source: Data source code.
        @type source: int
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def set_timeseries(self, idx: Union[int, str], ts_id: str) -> None:
        """Set the timeseries data source for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param ts_id: Timeseries identifier.
        @type ts_id: str
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def set_filename(self, idx: Union[int, str], path: str, station_id: str) -> None:
        """Set the external rainfall file and station for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @param path: Path to the rainfall data file.
        @type path: str
        @param station_id: Station identifier within the file.
        @type station_id: str
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def get_rain_type(self, idx: Union[int, str]) -> int:
        """Return the rain type code for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @return: Rain type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...

    def get_data_source(self, idx: Union[int, str]) -> int:
        """Return the data source code for a gage.

        @param idx: Gage index (int) or gage ID (str).
        @type idx: Union[int, str]
        @return: Data source code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the gage ID is not found.
        """
        ...
