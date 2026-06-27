"""
Engine Lifecycle (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._solver`.
"""

from collections.abc import MutableMapping, MutableSequence
from datetime import datetime, timedelta
from os import PathLike
from typing import TYPE_CHECKING, Any, Callable, Iterator, NamedTuple, Optional, Union

from ._enums import EngineState, FlowUnits

if TYPE_CHECKING:
    # Imports for type-checking only — avoids cycles at runtime because the
    # collection modules import Solver themselves.
    from ._2d import Surface2D
    from ._climate import Climate
    from ._controls import Controls
    from ._edit import ModelEditor
    from ._forcing import Forcing
    from ._gages import Gages
    from ._hotstart import SaveSchedule
    from ._infrastructure import Infrastructure
    from ._inflows import Inflows
    from ._links import Links
    from ._massbalance import MassBalance
    from ._nodes import Nodes
    from ._pollutants import Pollutants
    from ._quality import Quality
    from ._spatial import Spatial
    from ._statistics import Statistics
    from ._subcatchments import Aquifers, Snowpacks, Subcatchments
    from ._tables import Patterns, Tables

# Re-exported exception hierarchy. Canonical definitions in :mod:`_exceptions`.
from ._exceptions import (
    BadHandleError as BadHandleError,
    BadIndexError as BadIndexError,
    BadParamError as BadParamError,
    CRSError as CRSError,
    DependencyError as DependencyError,
    EngineError as EngineError,
    FileError as FileError,
    HotStartError as HotStartError,
    LifecycleError as LifecycleError,
    NumericalError as NumericalError,
    ParseError as ParseError,
    PluginError as PluginError,
)

_PathLike = Union[str, PathLike[str]]

GEOPACKAGE_PLUGIN_ID: str = "org.hydrocouple.openswmm.plugins.geopackage"


# ---------------------------------------------------------------------------
# Event record
# ---------------------------------------------------------------------------


class Event(NamedTuple):
    start: datetime
    end: datetime


# ---------------------------------------------------------------------------
# View classes (returned by Solver.options / .userflags / .events)
# ---------------------------------------------------------------------------


class SimulationOptions(MutableMapping[str, str]):
    """View over the ``[OPTIONS]`` block. See :class:`Solver.options`."""

    ext: "_OptionsExtView"

    def __init__(self, solver: "Solver") -> None: ...

    # MutableMapping protocol
    def __getitem__(self, key: str) -> str: ...
    def __setitem__(self, key: str, value: Any) -> None: ...
    def __delitem__(self, key: str) -> None: ...
    def __iter__(self) -> Iterator[str]: ...
    def __len__(self) -> int: ...
    def __contains__(self, key: object) -> bool: ...

    # Typed shortcuts
    @property
    def start_datetime(self) -> datetime: ...
    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None: ...
    @property
    def end_datetime(self) -> datetime: ...
    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None: ...
    @property
    def report_start_datetime(self) -> datetime: ...
    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None: ...
    @property
    def routing_step(self) -> timedelta: ...
    @property
    def crs(self) -> str: ...


class _OptionsExtView(MutableMapping[str, str]):
    """Sub-mapping for the ``[OPTIONS_EXT]`` block."""

    def __init__(self, solver: "Solver") -> None: ...
    def __getitem__(self, key: str) -> str: ...
    def __setitem__(self, key: str, value: Any) -> None: ...
    def __delitem__(self, key: str) -> None: ...
    def __iter__(self) -> Iterator[str]: ...
    def __len__(self) -> int: ...


class UserFlagDef(NamedTuple):
    """A ``[USER_FLAGS]`` schema definition: ``(name, type, description)``.

    ``type`` is 0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING
    (see :class:`UserFlagType`).
    """

    name: str
    type: int
    description: str


class UserFlags(MutableMapping[str, Union[bool, int, float, str]]):
    """Typed user-flag mapping. See :class:`Solver.userflags`."""

    def __init__(self, solver: "Solver") -> None: ...
    def __getitem__(self, key: str) -> Union[bool, int, float, str]: ...
    def __setitem__(self, key: str, value: Union[bool, int, float, str]) -> None: ...
    def __delitem__(self, key: str) -> None: ...
    def __iter__(self) -> Iterator[str]: ...
    def __len__(self) -> int: ...
    def __contains__(self, key: object) -> bool: ...

    # [USER_FLAGS] schema definitions
    def define(self, name: str, type: int, description: str = "") -> None: ...
    def undefine(self, name: str) -> None: ...
    def definitions(self) -> list[UserFlagDef]: ...

    # [USER_FLAG_VALUES] per-object values
    def get_value(self, obj_type: str, obj_name: str, flag_name: str) -> Optional[str]: ...
    def set_value(self, obj_type: str, obj_name: str, flag_name: str, value: str) -> None: ...
    def clear_value(self, obj_type: str, obj_name: str, flag_name: str) -> None: ...


class EventsView(MutableSequence[Event]):
    """``[EVENTS]`` block as a ``MutableSequence``. See :class:`Solver.events`."""

    def __init__(self, solver: "Solver") -> None: ...
    def __len__(self) -> int: ...
    def __getitem__(self, idx: Any) -> Any: ...
    def __setitem__(self, idx: Any, value: Any) -> None: ...
    def __delitem__(self, idx: Any) -> None: ...
    def insert(self, idx: int, value: Any) -> None: ...
    def append(self, value: Any) -> None: ...
    def clear(self) -> None: ...


# ---------------------------------------------------------------------------
# Module-level free functions
# ---------------------------------------------------------------------------


def run(
    inp: _PathLike,
    rpt: Optional[_PathLike] = None,
    out: Optional[_PathLike] = None,
    *,
    plugin_lib: Optional[_PathLike] = None,
) -> None:
    """Run a SWMM simulation start-to-finish. Raises :class:`EngineError` on
    failure."""
    ...


def run_with_callback(
    inp: _PathLike,
    rpt: Optional[_PathLike] = None,
    out: Optional[_PathLike] = None,
    callback: Optional[Callable[[float], None]] = None,
    *,
    plugin_lib: Optional[_PathLike] = None,
) -> None:
    """Run a SWMM simulation with an optional progress callback. Raises
    :class:`EngineError` on failure."""
    ...


# ---------------------------------------------------------------------------
# Solver
# ---------------------------------------------------------------------------


class Solver:
    """SWMM engine lifecycle manager.

    Accepts ``str`` or :class:`pathlib.Path` for every file argument.

    .. code-block:: python

        from datetime import timedelta
        from openswmm.engine import Solver

        with Solver("model.inp", "model.rpt", "model.out") as s:
            for elapsed in s.steps():
                if elapsed >= timedelta(hours=24):
                    break
    """

    def __init__(
        self,
        inp: _PathLike = "",
        rpt: Optional[_PathLike] = None,
        out: Optional[_PathLike] = None,
        *,
        plugin_lib: Optional[_PathLike] = None,
    ) -> None: ...

    # ------------- Lifecycle (raise on failure) -------------
    def create(self) -> None: ...
    def open(self, plugin_lib: Optional[_PathLike] = None) -> None: ...
    def initialize(self) -> None: ...
    def start(self, save_results: bool = True) -> None: ...
    def step(self) -> timedelta: ...
    def stride(self, n_steps: int) -> timedelta: ...
    def end(self) -> None: ...
    def report(self) -> None: ...
    def close(self) -> None: ...
    def destroy(self) -> None: ...
    def run(self) -> None: ...

    # ------------- Iteration helpers -------------
    def steps(self) -> Iterator[timedelta]: ...
    def until(self, target: Union[datetime, timedelta]) -> timedelta: ...

    # ------------- Typed properties -------------
    @property
    def elapsed(self) -> timedelta: ...
    @property
    def state(self) -> EngineState: ...
    @property
    def handle(self) -> int: ...
    @property
    def generation(self) -> int: ...
    @property
    def routing_step(self) -> timedelta: ...
    @property
    def crs(self) -> str: ...
    @property
    def is_between_events(self) -> bool: ...

    @property
    def steady_state_skip(self) -> bool: ...
    @steady_state_skip.setter
    def steady_state_skip(self, value: bool) -> None: ...

    @property
    def start_datetime(self) -> datetime: ...
    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None: ...
    @property
    def end_datetime(self) -> datetime: ...
    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None: ...
    @property
    def report_start_datetime(self) -> datetime: ...
    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None: ...
    @property
    def current_datetime(self) -> datetime: ...
    @property
    def sim_start_time(self) -> datetime:
        """Resolved simulation start (``swmm_get_start_time``).

        @rtype: datetime
        """
        ...
    @property
    def sim_end_time(self) -> datetime:
        """Resolved simulation end (``swmm_get_end_time``).

        @rtype: datetime
        """
        ...
    @property
    def event_count(self) -> int:
        """Number of ``[EVENTS]`` entries on the model.

        @rtype: int
        """
        ...

    # ------------- Units -------------
    @property
    def flow_units(self) -> FlowUnits:
        """The model's flow-unit system, resolved from ``FLOW_UNITS``.

        @return: Active flow-unit system; read this to interpret the
            project-unit magnitudes returned by all getters.
        @rtype: L{FlowUnits}
        """
        ...
    @property
    def unit_system(self) -> str:
        """``'US'`` (CFS/GPM/MGD) or ``'SI'`` (CMS/LPS/MLD).

        @rtype: str
        """
        ...
    # ------------- Views -------------
    @property
    def options(self) -> SimulationOptions: ...
    @property
    def userflags(self) -> UserFlags: ...
    @property
    def events(self) -> EventsView: ...

    # ------------- Collection accessors (typed) -------------
    @property
    def nodes(self) -> "Nodes": ...
    @property
    def links(self) -> "Links": ...
    @property
    def subcatchments(self) -> "Subcatchments": ...
    @property
    def aquifers(self) -> "Aquifers": ...
    @property
    def snowpacks(self) -> "Snowpacks": ...
    @property
    def gages(self) -> "Gages": ...
    @property
    def pollutants(self) -> "Pollutants": ...
    @property
    def tables(self) -> "Tables": ...
    @property
    def patterns(self) -> "Patterns": ...
    @property
    def inflows(self) -> "Inflows": ...
    @property
    def controls(self) -> "Controls": ...
    @property
    def forcing(self) -> "Forcing": ...
    @property
    def climate(self) -> "Climate": ...
    @property
    def infrastructure(self) -> "Infrastructure": ...
    @property
    def spatial(self) -> "Spatial": ...
    @property
    def quality(self) -> "Quality": ...
    @property
    def statistics(self) -> "Statistics": ...
    @property
    def mass_balance(self) -> "MassBalance": ...
    @property
    def editor(self) -> "ModelEditor": ...
    @property
    def save_schedule(self) -> "SaveSchedule": ...
    @property
    def surface2d(self) -> "Surface2D": ...

    # ------------- Model write -------------
    def write(self, path: _PathLike) -> None: ...
    def write_with_plugin(self, path: _PathLike, output_plugin_id: str = ...) -> None: ...
    def write_geopackage(self, path: _PathLike, crs: Optional[str] = ...) -> None: ...

    # ------------- Runoff interface file -------------
    def open_runoff_interface_write(self, path: _PathLike) -> None: ...
    def open_runoff_interface_read(self, path: _PathLike) -> None: ...
    def save_runoff_step(self, dt: Union[timedelta, float]) -> None: ...
    def read_runoff_step(self) -> bool: ...
    def close_runoff_interface(self) -> None: ...

    # ------------- Callbacks -------------
    def set_step_begin_callback(
        self, callback: Optional[Callable[[float, float], None]]
    ) -> None: ...
    def set_step_end_callback(
        self, callback: Optional[Callable[[float, float], None]]
    ) -> None: ...
    def set_warning_callback(
        self, callback: Optional[Callable[[int, str], None]]
    ) -> None: ...
    def set_progress_callback(
        self, callback: Optional[Callable[[float], None]]
    ) -> None:
        """Register a progress callback receiving the elapsed fraction [0, 1].

        @param callback: ``(elapsed_frac: float) -> None`` or ``None`` to clear.
        """
        ...

    # ------------- Context manager -------------
    def __enter__(self) -> "Solver": ...
    def __exit__(
        self,
        exc_type: Optional[type],
        exc_val: Optional[BaseException],
        exc_tb: Optional[object],
    ) -> bool: ...
    def __repr__(self) -> str: ...
