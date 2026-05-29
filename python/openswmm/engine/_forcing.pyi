"""Type stubs for :mod:`openswmm.engine._forcing`."""

from typing import Union

from ._enums import ForcingMode, ForcingTarget
from ._solver import Solver


_Key = Union[int, str]


class Forcing:
    def __init__(self, solver: Solver) -> None: ...

    def node_lat_inflow(
        self, node: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...
    def node_head_boundary(
        self, node: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...
    def node_quality(
        self, node: _Key, pollutant: _Key, mass_rate: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...

    def link_flow(
        self, link: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...
    def link_setting(
        self, link: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...

    def subcatchment_rainfall(
        self, sub: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...
    def subcatchment_evap(
        self, sub: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...

    def gage_rainfall(
        self, gage: _Key, value: float, *,
        mode: ForcingMode = ..., persist: bool = ...,
    ) -> None: ...

    def clear(self, target: ForcingTarget, key: _Key) -> None: ...
    def clear_all(self) -> None: ...
