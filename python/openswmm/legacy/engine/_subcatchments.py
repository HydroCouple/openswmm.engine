"""Pythonic property-based access to SWMM subcatchments.

Provides :class:`LegacySubcatchments` (collection) and
:class:`LegacySubcatchment` (single element) wrappers with typed properties,
LID access via sub_index, and mass-balance documentation.
"""

from typing import Optional, TYPE_CHECKING

from ._solver import (
    SWMMObjects,
    SWMMSubcatchmentProperties as SP,
)

if TYPE_CHECKING:
    from ._solver import Solver
    from ._forcing_log import ExternalForcingLog


class LegacySubcatchment:
    """Property-based access to a single SWMM subcatchment."""

    __slots__ = ("_solver", "_index", "_name")

    def __init__(self, solver: "Solver", index: int, name: str = ""):
        self._solver = solver
        self._index = index
        self._name = name or str(index)

    def _get(self, prop, **kw) -> float:
        return self._solver.get_value(SWMMObjects.SUBCATCHMENT, prop, self._index, **kw)

    def _set(self, prop, value: float, **kw) -> None:
        self._solver.set_value(SWMMObjects.SUBCATCHMENT, prop, self._index, value, **kw)

    # --- identity ---
    @property
    def index(self) -> int:
        return self._index

    @property
    def name(self) -> str:
        return self._name

    # --- parameters (get/set) ---
    @property
    def area(self) -> float:
        """Subcatchment area (project area units)."""
        return self._get(SP.AREA)

    @property
    def width(self) -> float:
        """Characteristic width of overland flow."""
        return self._get(SP.WIDTH)

    @width.setter
    def width(self, value: float) -> None:
        self._set(SP.WIDTH, value)

    @property
    def slope(self) -> float:
        """Average surface slope (%)."""
        return self._get(SP.SLOPE)

    @slope.setter
    def slope(self, value: float) -> None:
        self._set(SP.SLOPE, value)

    @property
    def curb_length(self) -> float:
        """Total curb length."""
        return self._get(SP.CURB_LENGTH)

    @property
    def fraction_impervious(self) -> float:
        """Fraction of impervious area (0-1)."""
        return self._get(SP.FRACTION_IMPERVIOUS)

    @property
    def outlet_type(self) -> int:
        """Outlet type (0=node, 1=subcatchment)."""
        return int(self._get(SP.OUTLET_TYPE))

    @property
    def outlet_index(self) -> int:
        """Index of outlet node or subcatchment."""
        return int(self._get(SP.OUTLET_INDEX))

    @property
    def infiltration_model(self) -> int:
        """Infiltration model code (0=Horton, 1=ModHorton, 2=GreenAmpt, etc.)."""
        return int(self._get(SP.INFILTRATION_MODEL))

    # --- results (read-only during simulation) ---
    @property
    def rainfall(self) -> float:
        """Current rainfall intensity."""
        return self._get(SP.RAINFALL)

    @property
    def evaporation(self) -> float:
        """Current evaporation rate."""
        return self._get(SP.EVAPORATION)

    @property
    def infiltration(self) -> float:
        """Current infiltration rate."""
        return self._get(SP.INFILTRATION)

    @property
    def runoff(self) -> float:
        """Current runoff flow rate."""
        return self._get(SP.RUNOFF)

    # --- subarea properties (sub_index: 0=imperv, 1=perv) ---
    def get_subarea_mannings_n(self, sub_index: int) -> float:
        """Manning's n for a subarea. sub_index: 0=impervious, 1=pervious."""
        return self._get(SP.SUB_AREA_MANNINGS_N, sub_index=sub_index)

    def get_subarea_depression_storage(self, sub_index: int) -> float:
        """Depression storage for a subarea."""
        return self._get(SP.SUB_AREA_DEPRESSION_STORAGE, sub_index=sub_index)

    def get_subarea_runoff(self, sub_index: int) -> float:
        """Current runoff for a subarea."""
        return self._get(SP.SUB_AREA_RUNOFF, sub_index=sub_index)

    def get_subarea_depth(self, sub_index: int) -> float:
        """Current depth for a subarea."""
        return self._get(SP.SUB_AREA_DEPTH, sub_index=sub_index)

    # --- LID properties ---
    @property
    def lid_units_count(self) -> int:
        """Number of LID units in this subcatchment."""
        return int(self._get(SP.LID_UNITS_COUNT))

    def get_lid_unit_area(self, lid_index: int) -> float:
        """Area of a specific LID unit."""
        return self._get(SP.LID_UNIT_AREA, sub_index=lid_index)

    def get_lid_unit_full_width(self, lid_index: int) -> float:
        """Full top width of a specific LID unit."""
        return self._get(SP.LID_UNIT_FULL_WIDTH, sub_index=lid_index)

    def get_lid_unit_surface_depth(self, lid_index: int) -> float:
        """Surface depth of a specific LID unit."""
        return self._get(SP.LID_UNIT_SURFACE_DEPTH, sub_index=lid_index)

    def get_lid_unit_soil_moisture(self, lid_index: int) -> float:
        """Soil moisture of a specific LID unit."""
        return self._get(SP.LID_UNIT_SOIL_MOISTURE, sub_index=lid_index)

    # --- pollutants ---
    def get_pollutant_buildup(self, pollutant_index: int = 0) -> float:
        """Current pollutant buildup on subcatchment."""
        return self._get(SP.POLLUTANT_BUILDUP, sub_index=pollutant_index)

    def get_pollutant_runoff_concentration(self, pollutant_index: int = 0) -> float:
        """Current pollutant concentration in runoff."""
        return self._get(SP.POLLUTANT_RUNOFF_CONCENTRATION,
                         pollutant_index=pollutant_index)

    def get_pollutant_total_load(self, pollutant_index: int = 0) -> float:
        """Total pollutant load washed off."""
        return self._get(SP.POLLUTANT_TOTAL_LOAD, pollutant_index=pollutant_index)

    # --- mass-balance-aware setters ---
    def set_api_rainfall(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Override rainfall for this subcatchment.

        Mass balance impact:
            Overrides the rainfall used in the runoff calculation for this
            subcatchment.  The overridden value appears in the **runoff
            totals** under ``rainfall``.  Resets each timestep.

        :param value: Rainfall intensity in project rainfall units.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SP.API_RAINFALL, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="subcatchment",
                object_id=self._name,
                property_name="api_rainfall",
                value=value,
                mass_balance_category="runoff.rainfall",
            )

    def set_api_snowfall(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Override snowfall for this subcatchment.

        Mass balance impact:
            Overrides the snowfall used in the snow accumulation/melt
            calculation.  Affects the snow balance in runoff totals.

        :param value: Snowfall rate in project units.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SP.API_SNOWFALL, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="subcatchment",
                object_id=self._name,
                property_name="api_snowfall",
                value=value,
                mass_balance_category="runoff.snow",
            )

    def set_external_pollutant_buildup(
        self,
        value: float,
        pollutant_index: int = 0,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Add external pollutant buildup on this subcatchment.

        Mass balance impact:
            Adds pollutant mass to the surface.  This mass is available
            for washoff and appears in the quality mass balance.

        :param value: Mass to add in project mass units.
        :param pollutant_index: 0-based pollutant index.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SP.EXTERNAL_POLLUTANT_BUILDUP, value,
                  sub_index=pollutant_index)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="subcatchment",
                object_id=self._name,
                property_name=f"ext_pollutant_buildup[{pollutant_index}]",
                value=value,
                mass_balance_category="quality.buildup",
            )

    # --- statistics (after end()) ---
    @property
    def statistics(self) -> dict:
        """Cumulative subcatchment statistics (call after ``solver.end()``)."""
        return self._solver.get_subcatchment_statistics(self._index)

    def __repr__(self) -> str:
        return f"LegacySubcatchment({self._name!r}, index={self._index})"


class LegacySubcatchments:
    """Iterable collection of all SWMM subcatchments."""

    __slots__ = ("_solver", "_items")

    def __init__(self, solver: "Solver"):
        self._solver = solver
        count = solver.get_object_count(SWMMObjects.SUBCATCHMENT)
        names = solver.get_object_names(SWMMObjects.SUBCATCHMENT)
        self._items = [LegacySubcatchment(solver, i, names[i]) for i in range(count)]

    def __getitem__(self, key) -> LegacySubcatchment:
        if isinstance(key, str):
            idx = self._solver.get_object_index(SWMMObjects.SUBCATCHMENT, key)
            return self._items[idx]
        return self._items[key]

    def __len__(self) -> int:
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __repr__(self) -> str:
        return f"LegacySubcatchments(count={len(self._items)})"
