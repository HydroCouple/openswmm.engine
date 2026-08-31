"""Type stubs for :mod:`openswmm.engine._process_components`."""

from collections.abc import Iterator
from typing import NamedTuple, Union

from ._solver import Solver


_Key = Union[int, str]


class ProcessComponent(NamedTuple):
    component_index: int
    id: str
    config: str
    resolved: str


class ProcessComponents:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[ProcessComponent]: ...
    def __getitem__(self, key: _Key) -> ProcessComponent: ...
    def __contains__(self, component_id: object) -> bool: ...
    def get_index(self, component_id: str) -> int: ...
    def register(
        self, component_id: str, config_path: str = ...,
    ) -> ProcessComponent: ...
    def remove(self, key: _Key) -> None: ...
