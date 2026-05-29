"""
Subcatchment access (Pythonic v1 surface)
=========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._subcatchments`.
"""

from collections.abc import Iterator, MutableMapping
from typing import Any, Optional, Tuple, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import InfilModel
from ._gages import Gage
from ._nodes import Node
from ._solver import Solver


_Key = Union[int, str]


class SubcatchmentStatsView:
    precip: float
    runoff_vol: float
    max_runoff: float


class InfiltrationView:
    model: InfilModel
    horton: Tuple[float, float, float, float]
    green_ampt: Tuple[float, float, float]
    curve_number: float

    def set_horton(self, f0: float, fmin: float, decay: float, dry_time: float) -> None: ...
    def set_green_ampt(self, suction: float, conductivity: float, initial_deficit: float) -> None: ...
    def set_curve_number(self, cn: float) -> None: ...


class CoverageView(MutableMapping[str, float]):
    def __init__(self, sub: "Subcatchment") -> None: ...
    def __getitem__(self, key: _Key) -> float: ...
    def __setitem__(self, key: _Key, value: float) -> None: ...
    def __delitem__(self, key: _Key) -> None: ...
    def __iter__(self) -> Iterator[str]: ...
    def __len__(self) -> int: ...


class Subcatchment:
    """Single-subcatchment wrapper. See :class:`Subcatchments`."""

    # Identity
    id: str
    index: int
    solver: Solver

    # Geometry / properties
    area: float
    width: float
    slope: float
    imperv_pct: float
    n_imperv: float
    n_perv: float
    ds_imperv: float
    ds_perv: float

    # Topology
    gage: Gage
    outlet: Union[Node, "Subcatchment", None]

    # Runtime state
    runoff: float
    groundwater: float
    rainfall: float
    snow_depth: float
    evap: float
    infil: float

    # Sub-views
    stats: SubcatchmentStatsView
    infiltration: InfiltrationView
    coverage: CoverageView

    def __init__(self, solver: Solver, index: int) -> None: ...

    def set_outlet_node(self, node: Union[Node, _Key]) -> None: ...
    def set_outlet_subcatchment(self, sub: Union["Subcatchment", _Key]) -> None: ...

    def quality(self, pollutant: _Key) -> float: ...
    def ponded_quality(self, pollutant: _Key) -> float: ...
    def set_ponded_quality(self, pollutant: _Key, mass: float) -> None: ...

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: ...


class Subcatchments:
    """Indexable, iterable collection of :class:`Subcatchment` wrappers."""

    def __init__(self, solver: Solver) -> None: ...

    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Subcatchment]: ...
    def __getitem__(self, key: _Key) -> Subcatchment: ...
    def __contains__(self, key: object) -> bool: ...

    def get_index(self, sub_id: str) -> int: ...
    def get_id(self, idx: int) -> str: ...
    def add(self, sub_id: str) -> Subcatchment: ...
    def rename(self, key: _Key, new_id: str) -> None: ...

    runoffs: NDArray[Any]
    rainfalls: NDArray[Any]
    evaps: NDArray[Any]
    infils: NDArray[Any]
    snow_depths: NDArray[Any]
    ids: NDArray[Any]

    def qualities(self, pollutant: _Key) -> NDArray[Any]: ...
