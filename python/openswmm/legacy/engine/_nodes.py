"""Pythonic property-based access to SWMM nodes.

Provides :class:`LegacyNodes` (collection) and :class:`LegacyNode` (single
element) wrappers around the generic ``Solver.get_value`` / ``set_value``
interface with typed properties, docstrings, and mass-balance documentation.
"""

from typing import Dict, Optional, TYPE_CHECKING

from ._solver import (
    SWMMObjects,
    SWMMNodeProperties,
    SWMMNodeTypes,
)

if TYPE_CHECKING:
    from ._solver import Solver
    from ._forcing_log import ExternalForcingLog


class LegacyNode:
    """Property-based access to a single SWMM node."""

    __slots__ = ("_solver", "_index", "_name")

    def __init__(self, solver: "Solver", index: int, name: str = ""):
        self._solver = solver
        self._index = index
        self._name = name or str(index)

    def _get(self, prop: SWMMNodeProperties, **kw) -> float:
        return self._solver.get_value(SWMMObjects.NODE, prop, self._index, **kw)

    def _set(self, prop: SWMMNodeProperties, value: float, **kw) -> None:
        self._solver.set_value(SWMMObjects.NODE, prop, self._index, value, **kw)

    # --- identity ---
    @property
    def index(self) -> int:
        return self._index

    @property
    def name(self) -> str:
        return self._name

    @property
    def node_type(self) -> SWMMNodeTypes:
        return SWMMNodeTypes(int(self._get(SWMMNodeProperties.TYPE)))

    # --- parameters (get/set) ---
    @property
    def invert_elevation(self) -> float:
        """Node invert elevation (project length units)."""
        return self._get(SWMMNodeProperties.INVERT_ELEVATION)

    @invert_elevation.setter
    def invert_elevation(self, value: float) -> None:
        self._set(SWMMNodeProperties.INVERT_ELEVATION, value)

    @property
    def max_depth(self) -> float:
        """Maximum water depth above invert (length units)."""
        return self._get(SWMMNodeProperties.MAX_DEPTH)

    @property
    def surcharge_depth(self) -> float:
        """Additional depth allowed before flooding (length units)."""
        return self._get(SWMMNodeProperties.SURCHARGE_DEPTH)

    @property
    def ponded_area(self) -> float:
        """Surface area used to compute ponded volume (area units)."""
        return self._get(SWMMNodeProperties.PONDING_AREA)

    @property
    def initial_depth(self) -> float:
        """Initial water depth at start of simulation."""
        return self._get(SWMMNodeProperties.INITIAL_DEPTH)

    # --- results (read-only during simulation) ---
    @property
    def depth(self) -> float:
        """Current water depth above invert."""
        return self._get(SWMMNodeProperties.DEPTH)

    @property
    def head(self) -> float:
        """Current hydraulic head (invert + depth)."""
        return self._get(SWMMNodeProperties.HYDRAULIC_HEAD)

    @property
    def volume(self) -> float:
        """Current stored volume."""
        return self._get(SWMMNodeProperties.VOLUME)

    @property
    def lateral_inflow(self) -> float:
        """Current lateral inflow rate."""
        return self._get(SWMMNodeProperties.LATERAL_INFLOW)

    @property
    def total_inflow(self) -> float:
        """Current total inflow rate (lateral + upstream)."""
        return self._get(SWMMNodeProperties.TOTAL_INFLOW)

    @property
    def flooding(self) -> float:
        """Current flooding (overflow) rate."""
        return self._get(SWMMNodeProperties.FLOODING)

    # --- pollutants ---
    def get_pollutant_concentration(self, pollutant_index: int = 0) -> float:
        """Get pollutant concentration at this node.

        :param pollutant_index: 0-based pollutant index.
        """
        return self._get(SWMMNodeProperties.POLLUTANT_CONCENTRATION,
                         pollutant_index=pollutant_index)

    # --- mass-balance-aware setters ---
    def set_lateral_inflow(
        self,
        flow: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Prescribe an external lateral inflow at this node.

        Mass balance impact:
            Adds *flow* to the node's lateral inflow for the **current
            timestep only**.  The value resets each step — call again to
            sustain.  The injected volume appears in the routing mass
            balance under ``ex_inflow`` (external inflow).

        :param flow: Inflow rate in project flow units.  Positive = inflow.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMNodeProperties.LATERAL_INFLOW, flow)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="node",
                object_id=self._name,
                property_name="lateral_inflow",
                value=flow,
                mass_balance_category="routing.ex_inflow",
            )

    def set_pollutant_lateral_mass_flux(
        self,
        value: float,
        pollutant_index: int = 0,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Set pollutant lateral mass flux at this node.

        Mass balance impact:
            The mass flux = value (mass/time).  This mass is added to the
            quality routing mass balance.  Must be used together with
            :meth:`set_lateral_inflow` for the mass to be transported.

        :param value: Mass flux in project mass/time units.
        :param pollutant_index: 0-based pollutant index.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMNodeProperties.POLLUTANT_LATERAL_MASS_FLUX, value,
                  pollutant_index=pollutant_index)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="node",
                object_id=self._name,
                property_name=f"pollutant_lat_mass_flux[{pollutant_index}]",
                value=value,
                mass_balance_category="quality.external",
            )

    def set_depth(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Override current water depth at this node.

        Mass balance impact:
            Directly changes stored volume — affects storage balance.
            Use with care: the volume difference is *not* automatically
            tracked as an inflow or outflow.

        :param value: Depth in project length units.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMNodeProperties.DEPTH, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="node",
                object_id=self._name,
                property_name="depth",
                value=value,
                mass_balance_category="routing.storage_override",
            )

    # --- statistics (after end()) ---
    @property
    def statistics(self) -> dict:
        """Cumulative node statistics (call after ``solver.end()``)."""
        return self._solver.get_node_statistics(self._index)

    def __repr__(self) -> str:
        return f"LegacyNode({self._name!r}, index={self._index})"


class LegacyNodes:
    """Iterable collection of all SWMM nodes in the model."""

    __slots__ = ("_solver", "_nodes")

    def __init__(self, solver: "Solver"):
        self._solver = solver
        count = solver.get_object_count(SWMMObjects.NODE)
        names = solver.get_object_names(SWMMObjects.NODE)
        self._nodes = [LegacyNode(solver, i, names[i]) for i in range(count)]

    def __getitem__(self, key) -> LegacyNode:
        if isinstance(key, str):
            idx = self._solver.get_object_index(SWMMObjects.NODE, key)
            return self._nodes[idx]
        return self._nodes[key]

    def __len__(self) -> int:
        return len(self._nodes)

    def __iter__(self):
        return iter(self._nodes)

    def __repr__(self) -> str:
        return f"LegacyNodes(count={len(self._nodes)})"
