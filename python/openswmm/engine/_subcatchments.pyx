"""
Subcatchment Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Subcatchments` class provides access to subcatchment properties
during a simulation.

.. code-block:: python

    from openswmm.engine import Solver, Subcatchments

    with Solver("model.inp", "model.rpt", "model.out") as s:
        subcatchments = Subcatchments(s)
        while s.step():
            runoff = subcatchments.get_runoff("S1")      # by name
            subcatchments.set_rainfall(0, 0.5)            # or by index
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Subcatchments:
    """Access subcatchment properties during a simulation.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept either an integer index or a string
    subcatchment ID.  When a string is passed it is resolved via
    :meth:`get_index`.

    .. code-block:: python

        subcatchments = Subcatchments(solver)
        runoff = subcatchments.get_runoff(0)      # by index
        runoff = subcatchments.get_runoff("S1")   # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve *idx* to an integer index.

        :param idx: Integer index or string subcatchment ID.
        :returns: Integer index.
        :raises KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Subcatchment '{idx}' not found")
            return i
        return idx

    # ------------------------------------------------------------------
    # Identity
    # ------------------------------------------------------------------

    def count(self) -> int:
        """Return the number of subcatchments.

        :returns: Subcatchment count.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_subcatch_count(h)

    def get_index(self, str sc_id) -> int:
        """Return the index of a subcatchment by ID.

        :param sc_id: Subcatchment identifier.
        :returns: Index, or -1 if not found.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = sc_id.encode('utf-8')
        return swmm_subcatch_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a subcatchment by index.

        :param idx: Subcatchment index.
        :returns: Subcatchment ID string.
        :rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_subcatch_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ------------------------------------------------------------------
    # Property setters (BUILDING / OPENED)
    # ------------------------------------------------------------------

    def set_outlet(self, idx, int node_idx):
        """Set the outlet node for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param node_idx: Index of the outlet node.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_outlet(h, i, node_idx))

    def set_area(self, idx, double area):
        """Set the area of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param area: Subcatchment area (project area units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_area(h, i, area))

    def set_width(self, idx, double width):
        """Set the characteristic width of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param width: Overland flow width (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_width(h, i, width))

    def set_slope(self, idx, double slope):
        """Set the average surface slope of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param slope: Average slope (fraction).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_slope(h, i, slope))

    def set_imperv_pct(self, idx, double pct):
        """Set the percent imperviousness of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param pct: Percent imperviousness (0-100).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_imperv_pct(h, i, pct))

    def set_n_imperv(self, idx, double n):
        """Set Manning's N for the impervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param n: Manning's roughness coefficient.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_n_imperv(h, i, n))

    def set_n_perv(self, idx, double n):
        """Set Manning's N for the pervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param n: Manning's roughness coefficient.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_n_perv(h, i, n))

    def set_ds_imperv(self, idx, double ds):
        """Set the depression storage for the impervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param ds: Depression storage depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_ds_imperv(h, i, ds))

    def set_ds_perv(self, idx, double ds):
        """Set the depression storage for the pervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param ds: Depression storage depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_ds_perv(h, i, ds))

    def set_gage(self, idx, int gage_idx):
        """Set the rain gage assigned to a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param gage_idx: Index of the rain gage.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_gage(h, i, gage_idx))

    def set_outlet_subcatch(self, idx, int sc_idx):
        """Set the outlet subcatchment for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param sc_idx: Index of the outlet subcatchment.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_outlet_subcatch(h, i, sc_idx))

    # ------------------------------------------------------------------
    # Infiltration setters
    # ------------------------------------------------------------------

    def set_infil_horton(self, idx, double f0, double fmin, double decay, double dry_time):
        """Set Horton infiltration parameters for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param f0: Maximum infiltration rate.
        :param fmin: Minimum infiltration rate.
        :param decay: Decay constant.
        :param dry_time: Drying time.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_infil_horton(h, i, f0, fmin, decay, dry_time))

    def set_infil_green_ampt(self, idx, double suction, double conductivity, double initial_deficit):
        """Set Green-Ampt infiltration parameters for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param suction: Soil capillary suction.
        :param conductivity: Soil saturated hydraulic conductivity.
        :param initial_deficit: Initial moisture deficit.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_infil_green_ampt(h, i, suction, conductivity, initial_deficit))

    def set_infil_curve_number(self, idx, double cn):
        """Set SCS Curve Number infiltration parameter for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param cn: SCS curve number.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_infil_curve_number(h, i, cn))

    # ------------------------------------------------------------------
    # Property getters
    # ------------------------------------------------------------------

    def get_area(self, idx) -> float:
        """Return the area of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Subcatchment area (project area units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_area(h, i, &v))
        return v

    def get_imperv_pct(self, idx) -> float:
        """Return the percent imperviousness of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Percent imperviousness (0-100).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_imperv_pct(h, i, &v))
        return v

    def get_outlet(self, idx) -> int:
        """Return the outlet node index for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Outlet node index.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_subcatch_get_outlet(h, i, &v))
        return v

    def get_width(self, idx) -> float:
        """Return the characteristic width of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Overland flow width (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_width(h, i, &v))
        return v

    def get_slope(self, idx) -> float:
        """Return the average surface slope of a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Average slope (fraction).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_slope(h, i, &v))
        return v

    def get_n_imperv(self, idx) -> float:
        """Return Manning's N for the impervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Manning's roughness coefficient.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_n_imperv(h, i, &v))
        return v

    def get_n_perv(self, idx) -> float:
        """Return Manning's N for the pervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Manning's roughness coefficient.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_n_perv(h, i, &v))
        return v

    def get_ds_imperv(self, idx) -> float:
        """Return the depression storage for the impervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Depression storage depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_ds_imperv(h, i, &v))
        return v

    def get_ds_perv(self, idx) -> float:
        """Return the depression storage for the pervious area.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Depression storage depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_ds_perv(h, i, &v))
        return v

    def get_gage(self, idx) -> int:
        """Return the rain gage index assigned to a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Rain gage index.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_subcatch_get_gage(h, i, &v))
        return v

    def get_outlet_subcatch(self, idx) -> int:
        """Return the outlet subcatchment index for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Outlet subcatchment index.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_subcatch_get_outlet_subcatch(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Infiltration getters
    # ------------------------------------------------------------------

    def get_infil_model(self, idx) -> int:
        """Return the infiltration model type for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Infiltration model type code.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_subcatch_get_infil_model(h, i, &v))
        return v

    def get_infil_horton(self, idx) -> tuple:
        """Return Horton infiltration parameters for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Tuple of (f0, fmin, decay, dry_time).
        :rtype: tuple
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double f0 = 0.0
        cdef double fmin = 0.0
        cdef double decay = 0.0
        cdef double dry_time = 0.0
        _check(swmm_subcatch_get_infil_horton(h, i, &f0, &fmin, &decay, &dry_time))
        return (f0, fmin, decay, dry_time)

    def get_infil_green_ampt(self, idx) -> tuple:
        """Return Green-Ampt infiltration parameters for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Tuple of (suction, conductivity, deficit).
        :rtype: tuple
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double suction = 0.0
        cdef double conductivity = 0.0
        cdef double deficit = 0.0
        _check(swmm_subcatch_get_infil_green_ampt(h, i, &suction, &conductivity, &deficit))
        return (suction, conductivity, deficit)

    def get_infil_curve_number(self, idx) -> float:
        """Return the SCS Curve Number for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: SCS curve number.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_infil_curve_number(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    def get_stat_precip(self, idx) -> float:
        """Return the total precipitation volume for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Precipitation volume.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_precip(h, i, &v))
        return v

    def get_stat_runoff_vol(self, idx) -> float:
        """Return the total runoff volume for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Runoff volume.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_runoff_vol(h, i, &v))
        return v

    def get_stat_max_runoff(self, idx) -> float:
        """Return the maximum runoff rate for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Maximum runoff rate.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_max_runoff(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Coverage
    # ------------------------------------------------------------------

    def set_coverage(self, idx, int lu_idx, double fraction):
        """Set the land-use coverage fraction for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param lu_idx: Land-use index.
        :param fraction: Coverage fraction (0-1).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_coverage(h, i, lu_idx, fraction))

    def get_coverage(self, idx, int lu_idx) -> float:
        """Return the land-use coverage fraction for a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param lu_idx: Land-use index.
        :returns: Coverage fraction (0-1).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_coverage(h, i, lu_idx, &v))
        return v

    # ------------------------------------------------------------------
    # Hydraulic state (runtime getters)
    # ------------------------------------------------------------------

    def get_runoff(self, idx) -> float:
        """Return the current runoff rate from a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Runoff rate (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_runoff(h, i, &v))
        return v

    def get_groundwater(self, idx) -> float:
        """Return the groundwater outflow from a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Groundwater flow rate (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_groundwater(h, i, &v))
        return v

    def get_rainfall(self, idx) -> float:
        """Return the current rainfall rate on a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Rainfall rate (project rainfall units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_rainfall(h, i, &v))
        return v

    def get_snow_depth(self, idx) -> float:
        """Return the current snow depth on a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Snow depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_snow_depth(h, i, &v))
        return v

    def get_evap(self, idx) -> float:
        """Return the current evaporation rate from a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Evaporation rate (project length units per time).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_evap(h, i, &v))
        return v

    def get_infil(self, idx) -> float:
        """Return the current infiltration rate on a subcatchment.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :returns: Infiltration rate (project length units per time).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_infil(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Runtime forcing
    # ------------------------------------------------------------------

    def set_rainfall(self, idx, double rainfall):
        """Override rainfall on a subcatchment for the current timestep.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param rainfall: Rainfall rate (project rainfall units).
                         Negative value reverts to gage-driven.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_subcatch_set_rainfall(h, i, rainfall))

    # ------------------------------------------------------------------
    # Quality
    # ------------------------------------------------------------------

    def get_quality(self, idx, int pollutant_idx) -> float:
        """Return the pollutant concentration in subcatchment runoff.

        :param idx: Subcatchment index (int) or subcatchment ID (str).
        :param pollutant_idx: Pollutant index.
        :returns: Concentration (project quality units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_subcatch_get_quality(h, i, pollutant_idx, &v))
        return v

    # ------------------------------------------------------------------
    # Bulk access
    # ------------------------------------------------------------------

    def get_runoff_bulk(self):
        """Return all subcatchment runoff rates as a NumPy array.

        Uses the bulk C API for a single ``memcpy`` -- much faster than
        calling :meth:`get_runoff` in a loop.

        :returns: Array of shape ``(n_subcatchments,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_subcatch_get_runoff_bulk(h, &buf[0], n))
        return buf

    def get_quality_bulk(self, int pollutant_idx):
        """Return all subcatchment pollutant concentrations as a NumPy array.

        :param pollutant_idx: Pollutant index.
        :returns: Array of shape ``(n_subcatchments,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_subcatch_get_quality_bulk(h, pollutant_idx, &buf[0], n))
        return buf
