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
Binary output file reader (Pythonic v1 surface)
===============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Type stubs for :mod:`openswmm.engine._output_reader`.
"""

from datetime import datetime, timedelta
from os import PathLike
from typing import Any, Dict, List, Optional, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import FlowUnits, OutLinkVar, OutNodeVar, OutSubcatchVar, OutSystemVar


_PathLike = Union[str, PathLike[str]]
_Key = Union[int, str]


class _OutputNodeStats:
    max_depth: float
    max_overflow: float
    vol_flooded: float
    time_flooded: float


class OutputReader:
    """Read a SWMM binary ``.out`` file."""

    # Metadata
    version: int
    flow_units: FlowUnits
    period_count: int
    report_step: timedelta
    start_datetime: datetime
    pollutant_count: int
    node_count: int
    link_count: int
    subcatchment_count: int
    error_code: int

    # Object ids
    node_ids: List[str]
    link_ids: List[str]
    subcatchment_ids: List[str]

    # Time axis
    period_times: NDArray[Any]  # dtype = datetime64[s]

    def __init__(self, path: _PathLike) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> "OutputReader": ...
    def __exit__(self, *args: Any) -> bool: ...
    def __repr__(self) -> str: ...

    # Per-period results
    def node_result(self, period: int, var: OutNodeVar) -> NDArray[Any]: ...
    def link_result(self, period: int, var: OutLinkVar) -> NDArray[Any]: ...
    def subcatchment_result(self, period: int, var: OutSubcatchVar) -> NDArray[Any]: ...
    def system_result(self, period: int, var: OutSystemVar) -> float: ...

    # Time series
    def node_series(
        self,
        node: _Key,
        var: OutNodeVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> NDArray[Any]: ...
    def link_series(
        self,
        link: _Key,
        var: OutLinkVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> NDArray[Any]: ...
    def subcatchment_series(
        self,
        sub: _Key,
        var: OutSubcatchVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> NDArray[Any]: ...
    def system_series(
        self,
        var: OutSystemVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> NDArray[Any]: ...

    # All-attribute dicts at one period
    def node_attributes(
        self, node: _Key, period: int
    ) -> Dict[Union[OutNodeVar, int], float]: ...
    def link_attributes(
        self, link: _Key, period: int
    ) -> Dict[Union[OutLinkVar, int], float]: ...
    def subcatchment_attributes(
        self, sub: _Key, period: int
    ) -> Dict[Union[OutSubcatchVar, int], float]: ...

    # Per-node summary stats
    def node_stats(self, node: _Key) -> _OutputNodeStats: ...
