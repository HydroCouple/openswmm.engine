"""Pythonic property-based access to SWMM rain gages."""

from typing import Optional, TYPE_CHECKING

from ._solver import (
    SWMMObjects,
    SWMMRainGageProperties,
)

if TYPE_CHECKING:
    from ._solver import Solver
    from ._forcing_log import ExternalForcingLog


class LegacyRainGage:
    """Property-based access to a single SWMM rain gage."""

    __slots__ = ("_solver", "_index", "_name")

    def __init__(self, solver: "Solver", index: int, name: str = ""):
        self._solver = solver
        self._index = index
        self._name = name or str(index)

    def _get(self, prop: SWMMRainGageProperties) -> float:
        return self._solver.get_value(SWMMObjects.RAIN_GAGE, prop, self._index)

    def _set(self, prop: SWMMRainGageProperties, value: float) -> None:
        self._solver.set_value(SWMMObjects.RAIN_GAGE, prop, self._index, value)

    @property
    def index(self) -> int:
        return self._index

    @property
    def name(self) -> str:
        return self._name

    @property
    def total_precipitation(self) -> float:
        """Current total precipitation (rainfall + snowfall)."""
        return self._get(SWMMRainGageProperties.GAGE_TOTAL_PRECIPITATION)

    @property
    def rainfall(self) -> float:
        """Current rainfall intensity."""
        return self._get(SWMMRainGageProperties.GAGE_RAINFALL)

    @property
    def snowfall(self) -> float:
        """Current snowfall intensity."""
        return self._get(SWMMRainGageProperties.GAGE_SNOWFALL)

    def set_rainfall(
        self,
        value: float,
        log: Optional["ExternalForcingLog"] = None,
    ) -> None:
        """Override rainfall at this gage.

        Mass balance impact:
            Overrides rainfall for **all subcatchments** linked to this gage.
            Appears in runoff totals under ``rainfall``.

        :param value: Rainfall rate in project rainfall units.
        :param log: Optional :class:`ExternalForcingLog` for audit.
        """
        self._set(SWMMRainGageProperties.GAGE_RAINFALL, value)
        if log is not None:
            log.record(
                sim_time=self._solver.current_datetime,
                object_type="rain_gage",
                object_id=self._name,
                property_name="rainfall",
                value=value,
                mass_balance_category="runoff.rainfall",
            )

    def __repr__(self) -> str:
        return f"LegacyRainGage({self._name!r}, index={self._index})"


class LegacyRainGages:
    """Iterable collection of all SWMM rain gages."""

    __slots__ = ("_solver", "_items")

    def __init__(self, solver: "Solver"):
        self._solver = solver
        count = solver.get_object_count(SWMMObjects.RAIN_GAGE)
        names = solver.get_object_names(SWMMObjects.RAIN_GAGE)
        self._items = [LegacyRainGage(solver, i, names[i]) for i in range(count)]

    def __getitem__(self, key) -> LegacyRainGage:
        if isinstance(key, str):
            idx = self._solver.get_object_index(SWMMObjects.RAIN_GAGE, key)
            return self._items[idx]
        return self._items[key]

    def __len__(self) -> int:
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __repr__(self) -> str:
        return f"LegacyRainGages(count={len(self._items)})"
