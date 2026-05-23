"""Pythonic property-based access to SWMM links.

Provides :class:`LegacyLinks` (collection) and :class:`LegacyLink` (single
element) wrappers with typed properties and mass-balance documentation.
"""

from typing import Optional, TYPE_CHECKING

from ._solver import (
    SWMMObjects,
    SWMMLinkProperties,
    SWMMLinkTypes,
)

if TYPE_CHECKING:
    from ._solver import Solver
    from ._forcing_log import ExternalForcingLog


class LegacyLink:
    """Property-based access to a single SWMM link."""

    __slots__ = ("_solver", "_index", "_name")

    def __init__(self, solver: "Solver", index: int, name: str = ""):
        self._solver = solver
        self._index = index
        self._name = name or str(index)

    def _get(self, prop: SWMMLinkProperties, **kw) -> float:
        return self._solver.get_value(SWMMObjects.LINK, prop, self._index, **kw)

    def _set(self, prop: SWMMLinkProperties, value: float, **kw) -> None:
        self._solver.set_value(SWMMObjects.LINK, prop, self._index, value, **kw)

    # --- identity ---
    @property
    def index(self) -> int:
        return self._index

    @property
    def name(self) -> str:
        return self._name

    @property
    def link_type(self) -> SWMMLinkTypes:
        return SWMMLinkTypes(int(self._get(SWMMLinkProperties.TYPE)))

    # --- geometry parameters (get/set) ---
    @property
    def length(self) -> float:
        """Conduit length."""
        return self._get(SWMMLinkProperties.LENGTH)

    @property
    def slope(self) -> float:
        """Conduit slope."""
        return self._get(SWMMLinkProperties.SLOPE)

    @property
    def full_depth(self) -> float:
        """Full (maximum) depth."""
        return self._get(SWMMLinkProperties.FULL_DEPTH)

    @property
    def full_flow(self) -> float:
        """Full-pipe flow rate."""
        return self._get(SWMMLinkProperties.FULL_FLOW)

    @property
    def start_node_offset(self) -> float:
        """Upstream invert offset."""
        return self._get(SWMMLinkProperties.START_NODE_OFFSET)

    @start_node_offset.setter
    def start_node_offset(self, value: float) -> None:
        self._set(SWMMLinkProperties.START_NODE_OFFSET, value)

    @property
    def end_node_offset(self) -> float:
        """Downstream invert offset."""
        return self._get(SWMMLinkProperties.END_NODE_OFFSET)

    @end_node_offset.setter
    def end_node_offset(self, value: float) -> None:
        self._set(SWMMLinkProperties.END_NODE_OFFSET, value)

    @property
    def initial_flow(self) -> float:
        """Initial flow at start of simulation."""
        return self._get(SWMMLinkProperties.INITIAL_FLOW)

    @initial_flow.setter
    def initial_flow(self, value: float) -> None:
        self._set(SWMMLinkProperties.INITIAL_FLOW, value)

    @property
    def flow_limit(self) -> float:
        """User-specified flow limit."""
        return self._get(SWMMLinkProperties.FLOW_LIMIT)

    @flow_limit.setter
    def flow_limit(self, value: float) -> None:
        self._set(SWMMLinkProperties.FLOW_LIMIT, value)

    # --- loss coefficients ---
    @property
    def inlet_loss(self) -> float:
        return self._get(SWMMLinkProperties.INLET_LOSS)

    @inlet_loss.setter
    def inlet_loss(self, value: float) -> None:
        self._set(SWMMLinkProperties.INLET_LOSS, value)

    @property
    def outlet_loss(self) -> float:
        return self._get(SWMMLinkProperties.OUTLET_LOSS)

    @outlet_loss.setter
    def outlet_loss(self, value: float) -> None:
        self._set(SWMMLinkProperties.OUTLET_LOSS, value)

    @property
    def average_loss(self) -> float:
        return self._get(SWMMLinkProperties.AVERAGE_LOSS)

    @average_loss.setter
    def average_loss(self, value: float) -> None:
        self._set(SWMMLinkProperties.AVERAGE_LOSS, value)

    @property
    def seepage_rate(self) -> float:
        return self._get(SWMMLinkProperties.SEEPAGE_RATE)

    # --- results (read-only during simulation) ---
    @property
    def flow(self) -> float:
        """Current flow rate."""
        return self._get(SWMMLinkProperties.FLOW)

    @property
    def depth(self) -> float:
        """Current flow depth."""
        return self._get(SWMMLinkProperties.DEPTH)

    @property
    def velocity(self) -> float:
        """Current flow velocity."""
        return self._get(SWMMLinkProperties.VELOCITY)

    @property
    def volume(self) -> float:
        """Current volume in link."""
        return self._get(SWMMLinkProperties.VOLUME)

    @property
    def capacity(self) -> float:
        """Current capacity utilization (0-1)."""
        return self._get(SWMMLinkProperties.CAPACITY)

    @property
    def setting(self) -> float:
        """Current link control setting (0-1 for pumps/orifices/weirs)."""
        return self._get(SWMMLinkProperties.SETTING)

    # --- pollutants ---
    def get_pollutant_concentration(self, pollutant_index: int = 0) -> float:
        return self._get(SWMMLinkProperties.POLLUTANT_CONCENTRATION,
                         pollutant_index=pollutant_index)

    # --- mass-balance-aware setters ---
    def set_setting(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Set the link control setting.

        Mass balance impact:
            Changes flow capacity — affects routing.  For pumps this
            controls on/off (0 or 1).  For orifices/weirs it controls
            the fraction open (0-1).  For conduits this has no direct
            effect unless used with inlet control.

        :param value: Setting value (typically 0-1).
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMLinkProperties.SETTING, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="link",
                object_id=self._name,
                property_name="setting",
                value=value,
                mass_balance_category="routing.flow_capacity",
            )

    def set_flow(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Directly override the flow in this link.

        Mass balance impact:
            **Bypasses hydraulic routing** — the specified flow is used
            regardless of head difference.  Use with extreme care as
            this can break mass balance if the override is inconsistent
            with node volumes.

        :param value: Flow rate in project flow units.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMLinkProperties.FLOW, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="link",
                object_id=self._name,
                property_name="flow",
                value=value,
                mass_balance_category="routing.flow_override",
            )

    # --- statistics (after end()) ---
    @property
    def statistics(self) -> dict:
        """Cumulative link statistics (call after ``solver.end()``)."""
        return self._solver.get_link_statistics(self._index)

    @property
    def pump_statistics(self) -> dict:
        """Cumulative pump statistics (only valid for PUMP links)."""
        return self._solver.get_pump_statistics(self._index)

    def __repr__(self) -> str:
        return f"LegacyLink({self._name!r}, index={self._index})"


class LegacyLinks:
    """Iterable collection of all SWMM links in the model."""

    __slots__ = ("_solver", "_links")

    def __init__(self, solver: "Solver"):
        self._solver = solver
        count = solver.get_object_count(SWMMObjects.LINK)
        names = solver.get_object_names(SWMMObjects.LINK)
        self._links = [LegacyLink(solver, i, names[i]) for i in range(count)]

    def __getitem__(self, key) -> LegacyLink:
        if isinstance(key, str):
            idx = self._solver.get_object_index(SWMMObjects.LINK, key)
            return self._links[idx]
        return self._links[key]

    def __len__(self) -> int:
        return len(self._links)

    def __iter__(self):
        return iter(self._links)

    def __repr__(self) -> str:
        return f"LegacyLinks(count={len(self._links)})"
