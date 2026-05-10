"""
Subcatchment Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._subcatchments`.

The :class:`Subcatchments` class provides access to subcatchment properties
during a simulation.
"""

from typing import Tuple, Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Subcatchments:
    """Access subcatchment properties during a simulation.

    All per-element methods accept either an integer index or a string
    subcatchment ID.  When a string is passed it is resolved via
    L{get_index}.

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Subcatchments

        with Solver("model.inp", "model.rpt", "model.out") as s:
            sc = Subcatchments(s)
            runoff = sc.get_runoff(0)      # by index
            runoff = sc.get_runoff("S1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string subcatchment ID.
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
        """Return the number of subcatchments in the model.

        @return: Subcatchment count.
        @rtype: int
        """
        ...

    def get_index(self, sc_id: str) -> int:
        """Return the integer index of a subcatchment by its string ID.

        @param sc_id: Subcatchment identifier.
        @type sc_id: str
        @return: Index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a subcatchment by index.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Subcatchment ID string.
        @rtype: str
        """
        ...

    def add(self, sc_id: str) -> int:
        """Add a subcatchment to the model (OPENED-state editing).

        Wraps C{swmm_subcatch_add}. Valid in C{BUILDING} or C{OPENED} state.

        @param sc_id: Unique subcatchment identifier.
        @type sc_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # ====================================================================
    # Property setters (BUILDING / OPENED)
    # ====================================================================

    def set_outlet(self, idx: Union[int, str], node_idx: int) -> None:
        """Set the outlet node for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param node_idx: Index of the outlet node.
        @type node_idx: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_area(self, idx: Union[int, str], area: float) -> None:
        """Set the area of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param area: Subcatchment area (project area units).
        @type area: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_width(self, idx: Union[int, str], width: float) -> None:
        """Set the characteristic width of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param width: Overland flow width (project length units).
        @type width: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_slope(self, idx: Union[int, str], slope: float) -> None:
        """Set the average surface slope of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param slope: Average slope (fraction).
        @type slope: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_imperv_pct(self, idx: Union[int, str], pct: float) -> None:
        """Set the percent imperviousness of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param pct: Percent imperviousness (C{0}-C{100}).
        @type pct: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_n_imperv(self, idx: Union[int, str], n: float) -> None:
        """Set Manning's N for the impervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param n: Manning's roughness coefficient.
        @type n: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_n_perv(self, idx: Union[int, str], n: float) -> None:
        """Set Manning's N for the pervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param n: Manning's roughness coefficient.
        @type n: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_ds_imperv(self, idx: Union[int, str], ds: float) -> None:
        """Set the depression storage for the impervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param ds: Depression storage depth (project length units).
        @type ds: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_ds_perv(self, idx: Union[int, str], ds: float) -> None:
        """Set the depression storage for the pervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param ds: Depression storage depth (project length units).
        @type ds: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_gage(self, idx: Union[int, str], gage_idx: int) -> None:
        """Set the rain gage assigned to a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param gage_idx: Index of the rain gage.
        @type gage_idx: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_outlet_subcatch(self, idx: Union[int, str], sc_idx: int) -> None:
        """Set the outlet subcatchment for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param sc_idx: Index of the outlet subcatchment.
        @type sc_idx: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Infiltration setters
    # ====================================================================

    def set_infil_horton(
        self,
        idx: Union[int, str],
        f0: float,
        fmin: float,
        decay: float,
        dry_time: float,
    ) -> None:
        """Set Horton infiltration parameters for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param f0: Maximum infiltration rate.
        @type f0: float
        @param fmin: Minimum infiltration rate.
        @type fmin: float
        @param decay: Decay constant.
        @type decay: float
        @param dry_time: Drying time.
        @type dry_time: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_infil_green_ampt(
        self,
        idx: Union[int, str],
        suction: float,
        conductivity: float,
        initial_deficit: float,
    ) -> None:
        """Set Green-Ampt infiltration parameters for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param suction: Soil capillary suction.
        @type suction: float
        @param conductivity: Soil saturated hydraulic conductivity.
        @type conductivity: float
        @param initial_deficit: Initial moisture deficit.
        @type initial_deficit: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_infil_curve_number(self, idx: Union[int, str], cn: float) -> None:
        """Set SCS Curve Number infiltration parameter for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param cn: SCS curve number.
        @type cn: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Property getters
    # ====================================================================

    def get_area(self, idx: Union[int, str]) -> float:
        """Return the area of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Subcatchment area (project area units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_imperv_pct(self, idx: Union[int, str]) -> float:
        """Return the percent imperviousness of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Percent imperviousness (C{0}-C{100}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_outlet(self, idx: Union[int, str]) -> int:
        """Return the outlet node index for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Outlet node index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_width(self, idx: Union[int, str]) -> float:
        """Return the characteristic width of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Overland flow width (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_slope(self, idx: Union[int, str]) -> float:
        """Return the average surface slope of a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Average slope (fraction).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_n_imperv(self, idx: Union[int, str]) -> float:
        """Return Manning's N for the impervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Manning's roughness coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_n_perv(self, idx: Union[int, str]) -> float:
        """Return Manning's N for the pervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Manning's roughness coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_ds_imperv(self, idx: Union[int, str]) -> float:
        """Return the depression storage for the impervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Depression storage depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_ds_perv(self, idx: Union[int, str]) -> float:
        """Return the depression storage for the pervious area.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Depression storage depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_gage(self, idx: Union[int, str]) -> int:
        """Return the rain gage index assigned to a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Rain gage index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_outlet_subcatch(self, idx: Union[int, str]) -> int:
        """Return the outlet subcatchment index for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Outlet subcatchment index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Infiltration getters
    # ====================================================================

    def get_infil_model(self, idx: Union[int, str]) -> int:
        """Return the infiltration model type for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Infiltration model type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_infil_horton(
        self, idx: Union[int, str]
    ) -> Tuple[float, float, float, float]:
        """Return Horton infiltration parameters for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(f0, fmin, decay, dry_time)}.
        @rtype: Tuple[float, float, float, float]
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_infil_green_ampt(
        self, idx: Union[int, str]
    ) -> Tuple[float, float, float]:
        """Return Green-Ampt infiltration parameters for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(suction, conductivity, deficit)}.
        @rtype: Tuple[float, float, float]
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_infil_curve_number(self, idx: Union[int, str]) -> float:
        """Return the SCS Curve Number for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: SCS curve number.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Statistics
    # ====================================================================

    def get_stat_precip(self, idx: Union[int, str]) -> float:
        """Return the total precipitation volume for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Precipitation volume.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_stat_runoff_vol(self, idx: Union[int, str]) -> float:
        """Return the total runoff volume for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Runoff volume.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_stat_max_runoff(self, idx: Union[int, str]) -> float:
        """Return the maximum runoff rate for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Maximum runoff rate.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Coverage
    # ====================================================================

    def set_coverage(self, idx: Union[int, str], lu_idx: int, fraction: float) -> None:
        """Set the land-use coverage fraction for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param lu_idx: Land-use index.
        @type lu_idx: int
        @param fraction: Coverage fraction (C{0}-C{1}).
        @type fraction: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_coverage(self, idx: Union[int, str], lu_idx: int) -> float:
        """Return the land-use coverage fraction for a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param lu_idx: Land-use index.
        @type lu_idx: int
        @return: Coverage fraction (C{0}-C{1}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Runoff state (runtime getters)
    # ====================================================================

    def get_runoff(self, idx: Union[int, str]) -> float:
        """Return the current runoff rate from a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Runoff rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_groundwater(self, idx: Union[int, str]) -> float:
        """Return the groundwater outflow from a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Groundwater flow rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_rainfall(self, idx: Union[int, str]) -> float:
        """Return the current rainfall rate on a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Rainfall rate (project rainfall units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_snow_depth(self, idx: Union[int, str]) -> float:
        """Return the current snow depth on a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Snow depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_evap(self, idx: Union[int, str]) -> float:
        """Return the current evaporation rate from a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Evaporation rate (project length units per time).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_infil(self, idx: Union[int, str]) -> float:
        """Return the current infiltration rate on a subcatchment.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @return: Infiltration rate (project length units per time).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Rainfall override
    # ====================================================================

    def set_rainfall(self, idx: Union[int, str], rainfall: float) -> None:
        """Override rainfall on a subcatchment for the current timestep.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param rainfall: Rainfall rate (project rainfall units).
            Negative value reverts to gage-driven.
        @type rainfall: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Pollutant/quality
    # ====================================================================

    def get_quality(self, idx: Union[int, str], pollutant_idx: int) -> float:
        """Return the pollutant concentration in subcatchment runoff.

        @param idx: Subcatchment index (int) or subcatchment ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Concentration (project quality units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def get_ponded_quality(self, idx: Union[int, str], pollutant_idx: int) -> float:
        """Return ponded quality mass for a subcatchment-pollutant pair.

        Ponded quality persists between wet/dry events and represents
        pollutant mass in standing water on the subcatchment surface.

        @param idx: Subcatchment index (int) or ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Ponded quality mass (project mass units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    def set_ponded_quality(
        self, idx: Union[int, str], pollutant_idx: int, mass: float
    ) -> None:
        """Set ponded quality mass for a subcatchment-pollutant pair.

        @param idx: Subcatchment index (int) or ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @param mass: Ponded quality mass (project mass units).
        @type mass: float
        @raise KeyError: If C{idx} is a string and the subcatchment is not found.
        """
        ...

    # ====================================================================
    # Bulk array access (numpy)
    # ====================================================================

    def get_runoff_bulk(self) -> npt.NDArray[np.float64]:
        """Return all subcatchment runoff rates as a NumPy array.

        Uses the bulk C API for a single C{memcpy} -- much faster than
        calling L{get_runoff} in a loop.

        @return: Array of shape C{(n_subcatchments,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_quality_bulk(self, pollutant_idx: int) -> npt.NDArray[np.float64]:
        """Return all subcatchment pollutant concentrations as a NumPy array.

        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Array of shape C{(n_subcatchments,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...
