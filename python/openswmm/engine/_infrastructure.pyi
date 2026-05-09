"""
Infrastructure Access
=====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._infrastructure`.

The :class:`Infrastructure` class provides access to transects, streets,
inlets, and LID controls/usage within the model.
"""

from ._solver import Solver


class Infrastructure:
    """Access transects, streets, inlets, and LID controls.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Infrastructure

        with Solver("model.inp", "model.rpt", "model.out") as s:
            infra = Infrastructure(s)
            print(f"Transects: {infra.transect_count()}")
            print(f"Streets:   {infra.street_count()}")
            print(f"LIDs:      {infra.lid_count()}")
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # Transects
    # ------------------------------------------------------------------

    def transect_count(self) -> int:
        """Return the number of transects in the model.

        Returns:
            Transect count.
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

        Args:
            idx: Transect index.
            n_left: Left overbank roughness (Manning's n).
            n_right: Right overbank roughness (Manning's n).
            n_channel: Channel roughness (Manning's n).
        """
        ...

    def transect_add_station(
        self, idx: int, station: float, elevation: float
    ) -> None:
        """Add a station-elevation point to a transect.

        Args:
            idx: Transect index.
            station: Station (horizontal distance).
            elevation: Elevation at this station.
        """
        ...

    # ------------------------------------------------------------------
    # Streets
    # ------------------------------------------------------------------

    def street_count(self) -> int:
        """Return the number of streets in the model.

        Returns:
            Street count.
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

        Args:
            idx: Street index.
            t_crown: Crown width.
            h_curb: Curb height.
            sx: Cross slope.
            n_road: Road roughness (Manning's n).
            gutter_depres: Gutter depression depth.
            gutter_width: Gutter width.
            sides: Number of sides (1 or 2).
            back_width: Backing width.
            back_slope: Backing slope.
            back_n: Backing roughness.
        """
        ...

    # ------------------------------------------------------------------
    # Inlets
    # ------------------------------------------------------------------

    def inlet_count(self) -> int:
        """Return the number of inlets in the model.

        Returns:
            Inlet count.
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

        Args:
            idx: Inlet index.
            length: Inlet length.
            width: Inlet width.
            grate_type: Grate type string.
            open_area: Open area fraction.
            splash_veloc: Splash-over velocity.
        """
        ...

    # ------------------------------------------------------------------
    # LID Controls
    # ------------------------------------------------------------------

    def lid_count(self) -> int:
        """Return the number of LID controls in the model.

        Returns:
            LID control count.
        """
        ...

    def lid_set_surface(
        self, idx: int, storage: float, roughness: float, slope: float
    ) -> None:
        """Set LID surface-layer parameters.

        Args:
            idx: LID index.
            storage: Surface storage depth.
            roughness: Surface roughness (Manning's n).
            slope: Surface slope.
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

        Args:
            idx: LID index.
            thick: Soil thickness.
            porosity: Porosity.
            fc: Field capacity.
            wp: Wilting point.
            ksat: Saturated hydraulic conductivity.
            kslope: Conductivity slope.
        """
        ...

    def lid_set_storage(
        self, idx: int, thick: float, void_frac: float, ksat: float
    ) -> None:
        """Set LID storage-layer parameters.

        Args:
            idx: LID index.
            thick: Storage thickness.
            void_frac: Void fraction.
            ksat: Saturated hydraulic conductivity.
        """
        ...

    def lid_set_drain(
        self, idx: int, coeff: float, expon: float, offset: float
    ) -> None:
        """Set LID underdrain parameters.

        Args:
            idx: LID index.
            coeff: Drain coefficient.
            expon: Drain exponent.
            offset: Drain offset height.
        """
        ...

    # ------------------------------------------------------------------
    # LID Usage
    # ------------------------------------------------------------------

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
        """Assign a LID control to a subcatchment.

        Args:
            subcatch_idx: Subcatchment index.
            lid_idx: LID control index.
            number: Number of LID units.
            area: Area of each unit.
            width: Top width of overland flow.
            init_sat: Initial saturation (0.0–1.0).
            from_imperv: Fraction of impervious area treated (0.0–1.0).
        """
        ...
