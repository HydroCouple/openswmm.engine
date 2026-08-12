# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Rain gage access (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Type stubs for :mod:`openswmm.engine._gages`.
"""

from collections.abc import Iterator
from datetime import timedelta
from typing import Any, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import GageDataSource, GageRainType
from ._solver import Solver


_Key = Union[int, str]


class Gage:
    id: str
    index: int
    solver: Solver
    rain_type: GageRainType
    data_source: GageDataSource
    scale_factor: float
    snow_factor: float
    rain_interval: float
    rain_units: int
    timeseries: str
    station_id: str
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

    rainfalls: NDArray[Any]
    ids: NDArray[Any]
