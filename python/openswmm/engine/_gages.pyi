"""
Rain gage access (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._gages`.
"""

from collections.abc import Iterator
from datetime import timedelta
from typing import Union

import numpy as np

from ._enums import GageDataSource, GageRainType
from ._solver import Solver


_Key = Union[int, str]


class Gage:
    id: str
    index: int
    solver: Solver
    rain_type: GageRainType
    data_source: GageDataSource
    rainfall: float

    def __init__(self, solver: Solver, index: int) -> None: ...
    def set_rain_interval(self, seconds: Union[float, timedelta]) -> None: ...
    def set_timeseries(self, ts_id: str) -> None: ...
    def set_file(self, path: str, station_id: str) -> None: ...

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: ...


class Gages:
    def __init__(self, solver: Solver) -> None: ...

    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Gage]: ...
    def __getitem__(self, key: _Key) -> Gage: ...
    def __contains__(self, key: object) -> bool: ...

    def get_index(self, gage_id: str) -> int: ...
    def get_id(self, idx: int) -> str: ...
    def add(self, gage_id: str) -> Gage: ...
    def rename(self, key: _Key, new_id: str) -> None: ...

    rainfalls: np.ndarray
    ids: np.ndarray
