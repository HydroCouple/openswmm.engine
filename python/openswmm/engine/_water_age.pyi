"""Type stubs for :mod:`openswmm.engine._water_age`."""

from collections.abc import Iterator
from os import PathLike
from typing import NamedTuple, Union

from ._enums import WaterAgeSource
from ._solver import Solver


_Node = Union[int, str]
_Source = Union[WaterAgeSource, int, str]
_PathLike = Union[str, PathLike[str]]


class WaterAgeOverride(NamedTuple):
    source: WaterAgeSource
    node_index: int
    hours: float


class _WaterAgeGlobals:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[WaterAgeSource]: ...
    def __getitem__(self, key: _Source) -> float: ...
    def __setitem__(self, key: _Source, hours: float) -> None: ...
    def __contains__(self, key: object) -> bool: ...


class _WaterAgeOverrides:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[WaterAgeOverride]: ...
    def __getitem__(self, row_index: int) -> WaterAgeOverride: ...
    def set(self, source: _Source, node: _Node, hours: float) -> None: ...
    def remove(self, source: _Source, node: _Node) -> None: ...


class WaterAge:
    def __init__(self, solver: Solver) -> None: ...

    enabled: bool
    globals: _WaterAgeGlobals
    node_overrides: _WaterAgeOverrides

    def save(self, path: _PathLike) -> None: ...
