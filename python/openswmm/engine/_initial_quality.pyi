"""Type stubs for :mod:`openswmm.engine._initial_quality`."""

from collections.abc import Iterator
from typing import NamedTuple, Optional, Union

from ._solver import Solver


_Key = Union[int, str]


class InitialQualityEntry(NamedTuple):
    is_link: bool
    elem_index: int
    constituent: str
    value: float


class InitialQuality:
    def __init__(self, solver: Solver) -> None: ...

    WATER_AGE: str
    TEMPERATURE: str

    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[InitialQualityEntry]: ...
    def __getitem__(self, row_index: int) -> InitialQualityEntry: ...
    def set(
        self, constituent: str, value: float, *,
        node: Optional[_Key] = ..., link: Optional[_Key] = ...,
    ) -> None: ...
    def remove(self, row_index: int) -> None: ...
