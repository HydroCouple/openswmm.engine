"""
Infrastructure Access
=====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.
    """

    def __init__(self, solver):
        self._solver = solver

    # ==================================================================
    # Transects
    # ==================================================================

    def transect_count(self) -> int:
        """Return the number of transects in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_transect_count(h)

    def transect_set_roughness(self, int idx, double n_left, double n_right,
                               double n_channel):
        """Set Manning's roughness values for a transect.

        :param idx: Transect index.
        :param n_left: Left overbank roughness.
        :param n_right: Right overbank roughness.
        :param n_channel: Channel roughness.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_roughness(h, idx, n_left, n_right, n_channel))

    def transect_add_station(self, int idx, double station, double elevation):
        """Add a station-elevation point to a transect.

        :param idx: Transect index.
        :param station: Station (horizontal distance).
        :param elevation: Elevation at this station.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_add_station(h, idx, station, elevation))

    # ==================================================================
    # Streets
    # ==================================================================

    def street_count(self) -> int:
        """Return the number of streets in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_street_count(h)

    def street_set_params(self, int idx, double t_crown, double h_curb,
                          double sx, double n_road, double gutter_depres,
                          double gutter_width, int sides, double back_width,
                          double back_slope, double back_n):
        """Set the geometric parameters for a street cross-section.

        :param idx: Street index.
        :param t_crown: Crown width.
        :param h_curb: Curb height.
        :param sx: Cross slope.
        :param n_road: Road roughness (Manning's n).
        :param gutter_depres: Gutter depression depth.
        :param gutter_width: Gutter width.
        :param sides: Number of sides (1 or 2).
        :param back_width: Backing width.
        :param back_slope: Backing slope.
        :param back_n: Backing roughness.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_street_set_params(h, idx, t_crown, h_curb, sx, n_road,
                                       gutter_depres, gutter_width, sides,
                                       back_width, back_slope, back_n))

    # ==================================================================
    # Inlets
    # ==================================================================

    def inlet_count(self) -> int:
        """Return the number of inlets in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_count(h)

    def inlet_set_params(self, int idx, double length, double width,
                         str grate_type, double open_area,
                         double splash_veloc):
        """Set the parameters for an inlet.

        :param idx: Inlet index.
        :param length: Inlet length.
        :param width: Inlet width.
        :param grate_type: Grate type string.
        :param open_area: Open area fraction.
        :param splash_veloc: Splash-over velocity.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_grate = grate_type.encode('utf-8')
        _check(swmm_inlet_set_params(h, idx, length, width, b_grate,
                                      open_area, splash_veloc))

    # ==================================================================
    # LID Controls
    # ==================================================================

    def lid_count(self) -> int:
        """Return the number of LID controls in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_lid_count(h)

    def lid_set_surface(self, int idx, double storage, double roughness,
                        double slope):
        """Set LID surface-layer parameters.

        :param idx: LID index.
        :param storage: Surface storage depth.
        :param roughness: Surface roughness (Manning's n).
        :param slope: Surface slope.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_surface(h, idx, storage, roughness, slope))

    def lid_set_soil(self, int idx, double thick, double porosity, double fc,
                     double wp, double ksat, double kslope):
        """Set LID soil-layer parameters.

        :param idx: LID index.
        :param thick: Soil thickness.
        :param porosity: Porosity.
        :param fc: Field capacity.
        :param wp: Wilting point.
        :param ksat: Saturated hydraulic conductivity.
        :param kslope: Conductivity slope.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_soil(h, idx, thick, porosity, fc, wp, ksat,
                                  kslope))

    def lid_set_storage(self, int idx, double thick, double void_frac,
                        double ksat):
        """Set LID storage-layer parameters.

        :param idx: LID index.
        :param thick: Storage thickness.
        :param void_frac: Void fraction.
        :param ksat: Saturated hydraulic conductivity.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_storage(h, idx, thick, void_frac, ksat))

    def lid_set_drain(self, int idx, double coeff, double expon, double offset):
        """Set LID underdrain parameters.

        :param idx: LID index.
        :param coeff: Drain coefficient.
        :param expon: Drain exponent.
        :param offset: Drain offset height.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_drain(h, idx, coeff, expon, offset))

    # ==================================================================
    # LID Usage
    # ==================================================================

    def lid_usage_add(self, int subcatch_idx, int lid_idx, int number,
                      double area, double width, double init_sat,
                      double from_imperv):
        """Assign a LID control to a subcatchment.

        :param subcatch_idx: Subcatchment index.
        :param lid_idx: LID control index.
        :param number: Number of LID units.
        :param area: Area of each unit.
        :param width: Top width of overland flow.
        :param init_sat: Initial saturation (0.0--1.0).
        :param from_imperv: Fraction of impervious area treated (0.0--1.0).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_usage_add(h, subcatch_idx, lid_idx, number, area,
                                   width, init_sat, from_imperv))
