from __future__ import annotations
"""Type stubs for :mod:`openswmm.engine._process_components`."""

import os
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
    @staticmethod
    def known() -> tuple[KnownProcessComponent, ...]: ...
    @staticmethod
    def libraries() -> tuple[ComponentLibrary, ...]:
        """HydroCouple component libraries found on the search path, refused
        ones included (with the reason in ``load_error``)."""
        ...
    @staticmethod
    def add_search_path(directory: Union[str, "os.PathLike[str]"]) -> None:
        """Scan *directory* for component libraries now, and keep it."""
        ...

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

class KnownProcessComponent(NamedTuple):
    id: str
    description: str
    implemented: bool


class ComponentLibrary(NamedTuple):
    """A HydroCouple component library the engine discovered."""
    id: str
    caption: str
    version: str
    path: str
    kind: str
    stamp: str
    load_error: str
