"""Type stubs for :mod:`openswmm.engine._inflows`."""

from typing import NamedTuple, Union

from ._solver import Solver


_Key = Union[int, str]


class RDIIEntry(NamedTuple):
    node_index: int
    uh_name: str
    area: float


class HydrographEntry(NamedTuple):
    uh_name: str
    month: int
    response: int
    r: float
    t: float
    k: float
    dmax: float
    drecov: float
    dinit: float


class HydrographGageEntry(NamedTuple):
    uh_name: str
    gage_name: str


class RDIIDecayEntry(NamedTuple):
    uh_name: str
    response: int
    k_dep: float
    k_0: float
    k_T: float
    T_ref: float
    theta_rec: float
    T_freeze: float


class Inflows:
    def __init__(self, solver: Solver) -> None: ...

    # External
    def add_external(
        self, node: _Key, constituent: str, *,
        ts_name: str = ..., type: str = ...,
        m_factor: float = ..., s_factor: float = ...,
        baseline: float = ..., pattern: str = ...,
    ) -> None: ...
    external_count: int

    # DWF
    def add_dwf(
        self, node: _Key, constituent: str, *,
        avg_value: float = ...,
        monthly_pattern: str = ..., daily_pattern: str = ...,
        hourly_pattern: str = ..., weekend_pattern: str = ...,
    ) -> None: ...
    dwf_count: int

    # RDII
    def add_rdii(self, node: _Key, uh_name: str, area: float) -> None: ...
    def get_rdii(self, idx: int) -> RDIIEntry: ...
    rdii_count: int

    # Hydrographs
    def add_hydrograph(
        self, uh_name: str, month: int, response: int,
        r: float, t: float, k: float, *,
        dmax: float = ..., drecov: float = ..., dinit: float = ...,
    ) -> None: ...
    def get_hydrograph(self, idx: int) -> HydrographEntry: ...
    hydrograph_count: int

    def add_hydrograph_gage(self, uh_name: str, gage_name: str) -> None: ...
    def get_hydrograph_gage(self, idx: int) -> HydrographGageEntry: ...
    hydrograph_gage_count: int
    hydrograph_group_count: int
    def get_hydrograph_group_id(self, idx: int) -> str: ...

    # RDII decay
    def add_rdii_decay(
        self, uh_name: str, response: int,
        k_dep: float, k_0: float, k_T: float,
        T_ref: float, theta_rec: float, T_freeze: float,
    ) -> None: ...
    def get_rdii_decay(self, idx: int) -> RDIIDecayEntry: ...
    rdii_decay_count: int
