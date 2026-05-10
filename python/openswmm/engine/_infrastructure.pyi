"""
Infrastructure Access
=====================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._infrastructure`.

The :class:`Infrastructure` class provides access to transects, streets,
inlets, and LID controls/usage within the model.
"""

from ._solver import Solver


class Infrastructure:
    """Access transects, streets, inlets, and LID controls.

    The class wraps the C{swmm_transect_*}, C{swmm_street_*},
    C{swmm_inlet_*}, C{swmm_lid_*}, and C{swmm_lid_usage_*} entry points
    of the OpenSWMM C API.

    Example::

        from openswmm.engine import Solver, Infrastructure

        with Solver("model.inp", "model.rpt", "model.out") as s:
            infra = Infrastructure(s)
            print(f"Transects: {infra.transect_count()}")
            print(f"Streets:   {infra.street_count()}")
            print(f"LIDs:      {infra.lid_count()}")

    @ivar _solver: The L{Solver} instance whose engine handle is used for
        every C API call.
    """

    def __init__(self, solver: Solver) -> None:
        """Construct an L{Infrastructure} accessor bound to a solver.

        @param solver: An active L{Solver} instance. The solver must remain
            alive for the lifetime of this object.
        @type solver: Solver
        """
        ...

    # ====================================================================
    # Transects
    # ====================================================================

    def transect_count(self) -> int:
        """Return the number of transects defined in the model.

        @return: Transect count.
        @rtype: int
        """
        ...

    def transect_set_roughness(
        self,
        idx: int,
        n_left: float,
        n_right: float,
        n_channel: float,
    ) -> None:
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
        ...

    def transect_add_station(
        self, idx: int, station: float, elevation: float
    ) -> None:
        """Append a station-elevation point to a transect.

        @param idx: Transect index.
        @type idx: int
        @param station: Station (horizontal distance) in project units.
        @type station: float
        @param elevation: Elevation at C{station} in project units.
        @type elevation: float
        @raise RuntimeError: If the C API rejects the point.
        """
        ...

    # ====================================================================
    # Streets
    # ====================================================================

    def street_count(self) -> int:
        """Return the number of street cross-sections in the model.

        @return: Street count.
        @rtype: int
        """
        ...

    def street_set_params(
        self,
        idx: int,
        t_crown: float,
        h_curb: float,
        sx: float,
        n_road: float,
        gutter_depres: float,
        gutter_width: float,
        sides: int,
        back_width: float,
        back_slope: float,
        back_n: float,
    ) -> None:
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
        ...

    # ====================================================================
    # Inlets
    # ====================================================================

    def inlet_count(self) -> int:
        """Return the number of inlets defined in the model.

        @return: Inlet count.
        @rtype: int
        """
        ...

    def inlet_set_params(
        self,
        idx: int,
        length: float,
        width: float,
        grate_type: str,
        open_area: float,
        splash_veloc: float,
    ) -> None:
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
        ...

    # ====================================================================
    # LIDs (Low-Impact Development)
    # ====================================================================

    def lid_count(self) -> int:
        """Return the number of LID controls in the model.

        @return: LID control count.
        @rtype: int
        """
        ...

    def lid_set_surface(
        self, idx: int, storage: float, roughness: float, slope: float
    ) -> None:
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
        ...

    def lid_set_soil(
        self,
        idx: int,
        thick: float,
        porosity: float,
        fc: float,
        wp: float,
        ksat: float,
        kslope: float,
    ) -> None:
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
        ...

    def lid_set_storage(
        self, idx: int, thick: float, void_frac: float, ksat: float
    ) -> None:
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
        ...

    def lid_set_drain(
        self, idx: int, coeff: float, expon: float, offset: float
    ) -> None:
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
        ...

    # ====================================================================
    # LID Usage
    # ====================================================================

    def lid_usage_add(
        self,
        subcatch_idx: int,
        lid_idx: int,
        number: int,
        area: float,
        width: float,
        init_sat: float,
        from_imperv: float,
    ) -> None:
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
        ...
