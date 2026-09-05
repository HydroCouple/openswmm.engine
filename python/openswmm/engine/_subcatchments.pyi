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
Subcatchment access (Pythonic v1 surface)
=========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Type stubs for :mod:`openswmm.engine._subcatchments`.
"""

from collections.abc import Iterator, MutableMapping
from typing import Any, List, NamedTuple, Optional, Tuple, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import AquiferParam, GwfType, InfilModel
from ._gages import Gage
from ._nodes import Node
from ._solver import Solver


_Key = Union[int, str]


class GroundwaterParams(NamedTuple):
    surf_elev: float
    a1: float
    b1: float
    a2: float
    b2: float
    a3: float
    tw: float
    hstar: float


class SubcatchmentStatsView:
    precip: float
    runoff_vol: float
    max_runoff: float


class InfiltrationView:
    model: InfilModel
    horton: Tuple[float, float, float, float]
    green_ampt: Tuple[float, float, float]
    curve_number: float
    curve_number_drying_time: float

    def set_horton(self, f0: float, fmin: float, decay: float, dry_time: float) -> None: ...
    def set_green_ampt(self, suction: float, conductivity: float, initial_deficit: float) -> None: ...
    def set_curve_number(self, cn: float, drying_time: float) -> None: ...


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
    tag: str
    index: int
    solver: Solver

    # Geometry / properties
    area: float
    width: float
    slope: float
    imperv_pct: float
    zero_imperv_pct: float
    n_imperv: float
    n_perv: float
    ds_imperv: float
    ds_perv: float

    # Topology
    gage: Gage
    outlet: Union[Node, "Subcatchment", None]

    # Groundwater / aquifer assignment
    aquifer: Optional[int]
    gw_node: Optional[int]
    gw_params: GroundwaterParams

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

    def set_gw_params(self, surf_elev: float, a1: float, b1: float,
                      a2: float, b2: float, a3: float,
                      tw: float, hstar: float) -> None: ...
    def set_gw_state(self, theta: float = ..., lower_depth: float = ...) -> None: ...
    def get_gw_state(self) -> tuple[float, float]: ...
    def set_snow_state(self, surface: int, swe: float = ..., fw: float = ...,
                       ati: float = ..., coldc: float = ...) -> None: ...
    def get_snow_state(self, surface: int) -> tuple[float, float, float, float]: ...

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

    def get_gwf_expression(self, key: _Key, gwf_type: Union[GwfType, int]) -> str: ...
    def set_gwf_expression(
        self, key: _Key, gwf_type: Union[GwfType, int], expression: Optional[str]
    ) -> None: ...
    def validate_gwf_expression(self, expression: str) -> Tuple[bool, str, int]: ...
    def gwf_variables(self) -> List[Tuple[str, str]]: ...
    def gwf_functions(self) -> List[str]: ...


class Aquifers:
    """Name-keyed collection of C{[AQUIFERS]} entries (C{solver.aquifers})."""

    def rename(self, key: _Key, new_id: str) -> None: ...
    def get_param(self, aquifer: _Key, param: Union[AquiferParam, int]) -> float: ...
    def set_param(
        self, aquifer: _Key, param: Union[AquiferParam, int], value: float
    ) -> None: ...
    def get_evap_pattern(self, aquifer: _Key) -> str: ...
    def set_evap_pattern(self, aquifer: _Key, name: Optional[str]) -> None: ...


class Snowpacks:
    """Name-keyed collection of C{[SNOWPACKS]} entries (C{solver.snowpacks})."""

    def rename(self, key: _Key, new_id: str) -> None: ...
    def set_plowable(
        self, snowpack: _Key, *,
        cmin: float, cmax: float, tbase: float, fwfrac: float,
        sd0: float, fw0: float, last: float,
    ) -> None: ...
    def get_plowable(self, snowpack: _Key) -> dict[str, float]: ...
    def set_impervious(
        self, snowpack: _Key, *,
        cmin: float, cmax: float, tbase: float, fwfrac: float,
        sd0: float, fw0: float, last: float,
    ) -> None: ...
    def get_impervious(self, snowpack: _Key) -> dict[str, float]: ...
    def set_pervious(
        self, snowpack: _Key, *,
        cmin: float, cmax: float, tbase: float, fwfrac: float,
        sd0: float, fw0: float, last: float,
    ) -> None: ...
    def get_pervious(self, snowpack: _Key) -> dict[str, float]: ...
    def set_removal(
        self, snowpack: _Key, *,
        dsnow: float, fout: float, fimp: float,
        fperv: float, fimelt: float, fsubcatch: float,
    ) -> None: ...
    def get_removal(self, snowpack: _Key) -> dict[str, float]: ...
    def set_removal_subcatch(self, snowpack: _Key, name: Optional[str]) -> None: ...
    def get_removal_subcatch(self, snowpack: _Key) -> str: ...
