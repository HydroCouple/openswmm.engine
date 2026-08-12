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
Link access (Pythonic v1 surface)
=================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Type stubs for :mod:`openswmm.engine._links`.
"""

from collections.abc import Iterator
from typing import Any, Tuple, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import LinkType, OrificeType, OutletRatingType, WeirType, XSectShape
from ._geometry import CrossSection
from ._nodes import Node
from ._solver import Solver
from ._xsect import XSectionGeometry


_Key = Union[int, str]


class LinkStatsView:
    max_flow: float
    max_velocity: float
    max_filling: float
    vol_flow: float
    surcharge_time: float
    pump_cycles: int
    pump_on_time: float
    pump_volume: float


class XSection:
    shape: XSectShape
    g1: float
    g2: float
    g3: float
    g4: float
    def as_tuple(self) -> Tuple[XSectShape, float, float, float, float]: ...
    def info(self) -> CrossSection: ...
    def geometry(self) -> XSectionGeometry: ...
    def __iter__(self) -> Iterator[Any]: ...


class PumpView:
    curve: int
    init_state: bool
    startup_depth: float
    shutoff_depth: float


class WeirView:
    type: WeirType
    crest_height: float
    discharge_coeff: float
    end_contractions: float


class OrificeView:
    type: OrificeType
    open_close_rate: float


class OutletView:
    rating_type: OutletRatingType
    expon: float


class Link:
    """Single-link wrapper. See :class:`Links` for the collection."""

    # Identity
    id: str
    tag: str
    index: int
    type: LinkType
    solver: Solver

    # Topology
    from_node: Node
    to_node: Node

    # Geometry
    length: float
    roughness: float
    slope: float
    offset_up: float
    offset_dn: float
    initial_flow: float
    max_flow: float

    # Cross-section — setter accepts XSection or (shape, g1, g2, g3, g4) tuple.
    @property
    def xsect(self) -> XSection: ...
    @xsect.setter
    def xsect(self, value: Union[XSection, Tuple[XSectShape, float, float, float, float]]) -> None: ...

    # Hydraulic state
    flow: float
    depth: float
    velocity: float
    capacity: float
    volume: float
    hyd_power: float

    # Control
    control_setting: float
    target_setting: float
    closed: bool

    # Common conduit knobs
    loss_coeff: Tuple[float, float, float]
    flap_gate: bool
    seep_rate: float
    culvert_code: int
    barrels: int

    # Sub-views
    stats: LinkStatsView
    pump: PumpView
    weir: WeirView
    orifice: OrificeView
    outlet: OutletView

    def __init__(self, solver: Solver, index: int) -> None: ...
    def set_nodes(self, from_node: Union[Node, _Key], to_node: Union[Node, _Key]) -> None: ...
    def quality(self, pollutant: _Key) -> float: ...

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: ...


class Links:
    """Indexable, iterable collection of :class:`Link` wrappers."""

    def __init__(self, solver: Solver) -> None: ...

    # Container protocol
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Link]: ...
    def __getitem__(self, key: _Key) -> Link: ...
    def __contains__(self, key: object) -> bool: ...

    # Identity lookups
    def get_index(self, link_id: str) -> int: ...
    def get_id(self, idx: int) -> str: ...
    def get_xsect_info(self, key: _Key) -> CrossSection: ...

    # Editing (bumps generation)
    def add(self, link_id: str, link_type: LinkType) -> Link: ...
    def pop_last(self, link_id: str) -> None: ...
    def rename(self, key: _Key, new_id: str) -> None: ...

    # Bulk numpy properties
    flows: NDArray[Any]
    depths: NDArray[Any]
    velocities: NDArray[Any]
    capacities: NDArray[Any]
    volumes: NDArray[Any]
    control_settings: NDArray[Any]
    target_settings: NDArray[Any]
    hyd_powers: NDArray[Any]
    ids: NDArray[Any]

    def qualities(self, pollutant: _Key) -> NDArray[Any]: ...
    def pump_stats(self) -> Tuple[NDArray[Any], NDArray[Any], NDArray[Any]]: ...

    def __repr__(self) -> str: ...
