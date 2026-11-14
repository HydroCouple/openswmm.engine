"""
Hot start files (Pythonic v1 surface)
=====================================

Type stubs for :mod:`openswmm.engine._hotstart`.
"""

from collections.abc import MutableSequence
from datetime import datetime
from os import PathLike
from typing import Any, List, NamedTuple, Optional, Union

from ._solver import Solver


_PathLike = Union[str, PathLike[str]]


class SaveScheduleEntry(NamedTuple):
    when: datetime
    path: str


class HotStartNodeRecord(NamedTuple):
    """One node record stored in a hot-start file, in internal units.

    ``depth`` and ``head`` in ft, ``volume`` in ft3, ``age`` in seconds
    (negative when water age is not tracked). ``head`` and ``volume`` are NaN
    when the file does not store them (a legacy file) or when
    :meth:`HotStart.set_node_state` marked them to be derived at restart.
    ``concentrations`` holds one value per :attr:`HotStart.species`.
    """
    id: str
    depth: float
    head: float
    volume: float
    age: float
    concentrations: tuple[float, ...]


class HotStartLinkRecord(NamedTuple):
    """One link record stored in a hot-start file, in internal units.

    ``flow`` in ft3/s (signed in link orientation), ``depth`` in ft,
    ``volume`` in ft3 (NaN when not stored or marked to be derived by
    :meth:`HotStart.set_link_state`), ``age`` in seconds (negative when not
    tracked). ``concentrations`` holds one value per :attr:`HotStart.species`.
    """
    id: str
    flow: float
    depth: float
    volume: float
    age: float
    concentrations: tuple[float, ...]


class HotStartSubcatchmentRecord(NamedTuple):
    """One subcatchment record stored in a hot-start file, in internal units.

    ``runoff`` and ``gw_flow`` in ft3/s, ``gw_upper_moisture`` as a fraction,
    ``gw_lower_depth`` in ft. A value the file does not hold is NaN.
    """
    id: str
    runoff: float
    gw_flow: float
    gw_upper_moisture: float
    gw_lower_depth: float


class HotStart:
    def __init__(self) -> None: ...

    @staticmethod
    def save_from(solver: Solver, path: _PathLike) -> None: ...

    @classmethod
    def open(cls, path: _PathLike) -> "HotStart": ...

    @classmethod
    def open_legacy(cls, model: Solver, path: _PathLike) -> "HotStart":
        """Open a legacy EPA SWMM 5 hot-start file for inspection.

        The file is read against ``model`` (OPENED or later): its
        subcatchment, node, link and pollutant counts must match, and records
        take the model's names. The handle is read-only; the model is not
        changed. Raises :class:`HotStartError` when the file cannot be read,
        is not a legacy file or was written for another model.
        """
        ...

    def close(self) -> None: ...
    def __enter__(self) -> "HotStart": ...
    def __exit__(self, *args: Any) -> bool: ...

    def apply(self, solver: Solver) -> None: ...

    @property
    def sim_datetime(self) -> datetime:
        """Simulation moment at which the hot-start state was captured: the
        saving run's start date plus the elapsed simulation time."""
        ...
    @property
    def start_datetime(self) -> datetime:
        """Start date and time of the run that saved the state."""
        ...
    @property
    def crs(self) -> Optional[str]:
        """Coordinate reference system string recorded in the file, or ``None``."""
        ...
    @property
    def node_count(self) -> int:
        """Number of nodes stored in the hot-start blob."""
        ...
    @property
    def link_count(self) -> int:
        """Number of links stored in the hot-start blob."""
        ...
    @property
    def subcatchment_count(self) -> int:
        """Number of subcatchments stored in the hot-start blob."""
        ...
    @property
    def contents(self) -> tuple[str, ...]:
        """State blocks the file holds: ``"SPECIES"``, ``"GROUNDWATER"``,
        ``"GW_TRANSPORT"``, ``"SNOW"``, ``"LID"``, ``"SURFACE"``,
        ``"ENGINE_CHECKPOINT"`` and ``"LEGACY"``."""
        ...
    @property
    def version(self) -> int:
        """File format version: the native record version, or 1-4 for a legacy file."""
        ...
    @property
    def saved_at(self) -> Optional[datetime]:
        """When a native file was saved (UTC), or ``None`` for a legacy file."""
        ...
    @property
    def end_datetime(self) -> Optional[datetime]:
        """End date and time of the saving run, or ``None`` for a legacy file."""
        ...
    @property
    def species(self) -> List[str]:
        """Species whose node and link concentrations the file holds."""
        ...
    @property
    def warnings(self) -> List[str]:
        """Warnings emitted while opening or applying the hot-start file."""
        ...

    def node_records(self) -> List[HotStartNodeRecord]:
        """Every stored node record, in file order."""
        ...
    def link_records(self) -> List[HotStartLinkRecord]:
        """Every stored link record, in file order."""
        ...
    def subcatchment_records(self) -> List[HotStartSubcatchmentRecord]:
        """Every stored subcatchment record, in file order."""
        ...

    def set_node_depth(self, node_id: str, depth: float) -> None: ...
    def set_node_head(self, node_id: str, head: float) -> None: ...
    def set_link_flow(self, link_id: str, flow: float) -> None: ...
    def set_link_depth(self, link_id: str, depth: float) -> None: ...
    def set_subcatchment_runoff(self, sub_id: str, runoff: float) -> None: ...
    def set_node_state(self, node_id: str, depth: float) -> None:
        """Set a node's depth (ft) so a restart derives its head and volume;
        written to the file on :meth:`close`."""
        ...
    def set_link_state(self, link_id: str, flow: float, depth: float) -> None:
        """Set a link's flow (ft3/s) and depth (ft) so a restart derives its
        volume; written to the file on :meth:`close`."""
        ...

    def __repr__(self) -> str: ...


class SaveSchedule(MutableSequence[SaveScheduleEntry]):
    def __init__(self, solver: Solver) -> None: ...

    def __len__(self) -> int: ...
    def __getitem__(self, idx: Any) -> Any: ...
    def __setitem__(self, idx: Any, value: Any) -> None: ...
    def __delitem__(self, idx: Any) -> None: ...
    def insert(self, idx: int, value: Any) -> None: ...
    def append(self, value: Any) -> None: ...
    def clear(self) -> None: ...
    def get_format(self, idx: int) -> str:
        """File format of entry *idx*: ``"AUTO"``, ``"LEGACY"`` or ``"NATIVE"``."""
        ...
    def set_format(self, idx: int, fmt: str) -> None:
        """Set the file format of entry *idx* (``"AUTO"``, ``"LEGACY"`` or ``"NATIVE"``)."""
        ...
    def legacy_omits(self) -> tuple[str, ...]:
        """Model state a legacy SWMM5 file has no place for and NATIVE keeps."""
        ...
