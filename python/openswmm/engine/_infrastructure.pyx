"""
Infrastructure Access
=====================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Infrastructure` class provides access to transects, streets,
inlets, and LID controls/usage within the model.

.. code-block:: python

    from openswmm.engine import Solver, Infrastructure

    with Solver("model.inp", "model.rpt", "model.out") as s:
        infra = Infrastructure(s)
        print(f"Transects: {infra.transect_count()}")
        print(f"Streets:   {infra.street_count()}")
        print(f"LIDs:      {infra.lid_count()}")
"""

# cython: language_level=3

from ._common cimport *


class Infrastructure:
    """Access transects, streets, inlets, and LID controls.

    The class wraps the C{swmm_transect_*}, C{swmm_street_*},
    C{swmm_inlet_*}, C{swmm_lid_*}, and C{swmm_lid_usage_*} entry points
    of the OpenSWMM C API.

    @ivar _solver: The L{Solver} instance whose engine handle is used for
        every C API call.
    """

    def __init__(self, solver):
        """Construct an L{Infrastructure} accessor bound to a solver.

        @param solver: An active L{openswmm.engine.Solver} instance. The
            solver must remain alive for the lifetime of this object.
        @type solver: Solver
        """
        self._solver = solver

    # ====================================================================
    # Transects
    # ====================================================================

    def transect_count(self) -> int:
        """Return the number of transects defined in the model.

        @return: Transect count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_transect_count(h)

    def transect_set_roughness(self, int idx, double n_left, double n_right,
                               double n_channel):
        """Set Manning's roughness values for a transect.

        @param idx: Transect index.
        @type idx: int
        @param n_left: Left overbank Manning's M{n}.
        @type n_left: float
        @param n_right: Right overbank Manning's M{n}.
        @type n_right: float
        @param n_channel: Channel Manning's M{n}.
        @type n_channel: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_roughness(h, idx, n_left, n_right, n_channel))

    def transect_add_station(self, int idx, double station, double elevation):
        """Append a station-elevation point to a transect.

        @param idx: Transect index.
        @type idx: int
        @param station: Station (horizontal distance) in project units.
        @type station: float
        @param elevation: Elevation at C{station} in project units.
        @type elevation: float
        @raise RuntimeError: If the C API rejects the point.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_add_station(h, idx, station, elevation))

    # ====================================================================
    # Streets
    # ====================================================================

    def street_count(self) -> int:
        """Return the number of street cross-sections in the model.

        @return: Street count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_street_count(h)

    def street_set_params(self, int idx, double t_crown, double h_curb,
                          double sx, double n_road, double gutter_depres,
                          double gutter_width, int sides, double back_width,
                          double back_slope, double back_n):
        """Set the geometric parameters for a street cross-section.

        @param idx: Street index.
        @type idx: int
        @param t_crown: Crown width.
        @type t_crown: float
        @param h_curb: Curb height.
        @type h_curb: float
        @param sx: Cross slope.
        @type sx: float
        @param n_road: Road Manning's M{n}.
        @type n_road: float
        @param gutter_depres: Gutter depression depth.
        @type gutter_depres: float
        @param gutter_width: Gutter width.
        @type gutter_width: float
        @param sides: Number of sides (C{1} or C{2}).
        @type sides: int
        @param back_width: Backing width.
        @type back_width: float
        @param back_slope: Backing slope.
        @type back_slope: float
        @param back_n: Backing Manning's M{n}.
        @type back_n: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_street_set_params(h, idx, t_crown, h_curb, sx, n_road,
                                       gutter_depres, gutter_width, sides,
                                       back_width, back_slope, back_n))

    # ====================================================================
    # Inlets
    # ====================================================================

    def inlet_count(self) -> int:
        """Return the number of inlets defined in the model.

        @return: Inlet count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_count(h)

    def inlet_set_params(self, int idx, double length, double width,
                         str grate_type, double open_area,
                         double splash_veloc):
        """Set the parameters for an inlet.

        @param idx: Inlet index.
        @type idx: int
        @param length: Inlet length.
        @type length: float
        @param width: Inlet width.
        @type width: float
        @param grate_type: Grate type identifier string (e.g. C{"P_BAR-50"}).
        @type grate_type: str
        @param open_area: Open area fraction.
        @type open_area: float
        @param splash_veloc: Splash-over velocity.
        @type splash_veloc: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_grate = grate_type.encode('utf-8')
        _check(swmm_inlet_set_params(h, idx, length, width, b_grate,
                                      open_area, splash_veloc))

    # ====================================================================
    # LIDs (Low-Impact Development)
    # ====================================================================

    def lid_count(self) -> int:
        """Return the number of LID controls in the model.

        @return: LID control count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_lid_count(h)

    def lid_set_surface(self, int idx, double storage, double roughness,
                        double slope):
        """Set LID surface-layer parameters.

        @param idx: LID control index.
        @type idx: int
        @param storage: Surface storage depth.
        @type storage: float
        @param roughness: Surface Manning's M{n}.
        @type roughness: float
        @param slope: Surface slope.
        @type slope: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_surface(h, idx, storage, roughness, slope))

    def lid_set_soil(self, int idx, double thick, double porosity, double fc,
                     double wp, double ksat, double kslope):
        """Set LID soil-layer parameters.

        @param idx: LID control index.
        @type idx: int
        @param thick: Soil thickness.
        @type thick: float
        @param porosity: Porosity (volume fraction).
        @type porosity: float
        @param fc: Field capacity (volume fraction).
        @type fc: float
        @param wp: Wilting point (volume fraction).
        @type wp: float
        @param ksat: Saturated hydraulic conductivity.
        @type ksat: float
        @param kslope: Conductivity slope (Green-Ampt parameter).
        @type kslope: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_soil(h, idx, thick, porosity, fc, wp, ksat,
                                  kslope))

    def lid_set_storage(self, int idx, double thick, double void_frac,
                        double ksat):
        """Set LID storage-layer parameters.

        @param idx: LID control index.
        @type idx: int
        @param thick: Storage thickness.
        @type thick: float
        @param void_frac: Void fraction.
        @type void_frac: float
        @param ksat: Saturated hydraulic conductivity.
        @type ksat: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_storage(h, idx, thick, void_frac, ksat))

    def lid_set_drain(self, int idx, double coeff, double expon, double offset):
        """Set LID underdrain parameters.

        @param idx: LID control index.
        @type idx: int
        @param coeff: Drain coefficient.
        @type coeff: float
        @param expon: Drain exponent.
        @type expon: float
        @param offset: Drain offset height.
        @type offset: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_drain(h, idx, coeff, expon, offset))

    # ====================================================================
    # LID Usage
    # ====================================================================

    def lid_usage_add(self, int subcatch_idx, int lid_idx, int number,
                      double area, double width, double init_sat,
                      double from_imperv):
        """Assign an LID control to a subcatchment.

        @param subcatch_idx: Subcatchment index.
        @type subcatch_idx: int
        @param lid_idx: LID control index.
        @type lid_idx: int
        @param number: Number of LID units placed.
        @type number: int
        @param area: Area of each LID unit.
        @type area: float
        @param width: Top width of overland flow surface.
        @type width: float
        @param init_sat: Initial saturation in the range C{0.0}-C{1.0}.
        @type init_sat: float
        @param from_imperv: Fraction of impervious area treated, in
            C{0.0}-C{1.0}.
        @type from_imperv: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_usage_add(h, subcatch_idx, lid_idx, number, area,
                                   width, init_sat, from_imperv))
