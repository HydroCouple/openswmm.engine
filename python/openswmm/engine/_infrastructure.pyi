"""Type stubs for :mod:`openswmm.engine._infrastructure`."""

from typing import Union

from ._enums import LidType
from ._solver import Solver


_Key = Union[int, str]


class Transects:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def add(self, transect_id: str) -> int: ...
    def set_roughness(
        self, idx: int, n_left: float, n_right: float, n_channel: float,
    ) -> None: ...
    def add_station(self, idx: int, station: float, elevation: float) -> None: ...


class Streets:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def add(self, street_id: str) -> int: ...
    def set_params(
        self, idx: int, *,
        t_crown: float, h_curb: float, sx: float, n_road: float,
        gutter_depres: float = ..., gutter_width: float = ...,
        sides: int = ...,
        back_width: float = ..., back_slope: float = ..., back_n: float = ...,
    ) -> None: ...


class Inlets:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def add(self, inlet_id: str, inlet_type: str) -> int: ...
    def set_params(
        self, idx: int, *,
        length: float = ..., width: float = ...,
        grate_type: str = ...,
        open_area: float = ..., splash_veloc: float = ...,
    ) -> None: ...


class LIDs:
    def __init__(self, solver: Solver) -> None: ...
    def __len__(self) -> int: ...
    def add(self, lid_id: str, lid_type: LidType) -> int: ...
    def set_surface(
        self, idx: int, *, storage: float, roughness: float, slope: float,
    ) -> None: ...
    def set_soil(
        self, idx: int, *,
        thick: float, porosity: float, fc: float,
        wp: float, ksat: float, kslope: float,
    ) -> None: ...
    def set_storage(
        self, idx: int, *, thick: float, void_frac: float, ksat: float,
    ) -> None: ...
    def set_drain(
        self, idx: int, *, coeff: float, expon: float, offset: float,
    ) -> None: ...
    def usage_add(
        self, subcatchment: _Key, lid: int, *,
        number: int, area: float, width: float,
        init_sat: float = ..., from_imperv: float = ...,
    ) -> None: ...


class Infrastructure:
    def __init__(self, solver: Solver) -> None: ...

    transects: Transects
    streets: Streets
    inlets: Inlets
    lids: LIDs
