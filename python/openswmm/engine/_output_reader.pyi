"""
Binary output file reader (Pythonic v1 surface)
===============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._output_reader`.
"""

from datetime import datetime, timedelta
from os import PathLike
from typing import Dict, List, Optional, Union

import numpy as np

from ._enums import FlowUnits, OutLinkVar, OutNodeVar, OutSubcatchVar, OutSystemVar


_PathLike = Union[str, PathLike]
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
    period_times: np.ndarray  # dtype = datetime64[s]

    def __init__(self, path: _PathLike) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> "OutputReader": ...
    def __exit__(self, *args) -> bool: ...
    def __repr__(self) -> str: ...

    # Per-period results
    def node_result(self, period: int, var: OutNodeVar) -> np.ndarray: ...
    def link_result(self, period: int, var: OutLinkVar) -> np.ndarray: ...
    def subcatchment_result(self, period: int, var: OutSubcatchVar) -> np.ndarray: ...
    def system_result(self, period: int, var: OutSystemVar) -> float: ...

    # Time series
    def node_series(
        self,
        node: _Key,
        var: OutNodeVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> np.ndarray: ...
    def link_series(
        self,
        link: _Key,
        var: OutLinkVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> np.ndarray: ...
    def subcatchment_series(
        self,
        sub: _Key,
        var: OutSubcatchVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> np.ndarray: ...
    def system_series(
        self,
        var: OutSystemVar,
        *,
        start: Optional[int] = ...,
        end: Optional[int] = ...,
    ) -> np.ndarray: ...

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
