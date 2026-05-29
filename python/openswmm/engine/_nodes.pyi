"""
Node access (Pythonic v1 surface)
=================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._nodes`.
"""

from collections.abc import Iterator
from typing import Any, Tuple, Union

import numpy as np
from numpy.typing import NDArray

from ._enums import NodeType, OutfallType
from ._solver import Solver


_Key = Union[int, str]


class NodeStatsView:
    max_depth: float
    max_overflow: float
    vol_flooded: float
    time_flooded: float


class StorageView:
    curve: int
    functional: Tuple[float, float, float]
    seep_rate: float
    exfil_params: Tuple[float, float, float]


class OutfallView:
    type: OutfallType
    param: float
    flap_gate: bool
    route_to: int

    def set_stage(self, stage: float) -> None: ...
    def set_tidal_curve(self, curve_idx: int) -> None: ...
    def set_timeseries(self, ts_idx: int) -> None: ...


class DividerView:
    type: int


class Node:
    """Single-node wrapper. See :class:`Nodes` for the collection."""

    # Identity
    id: str
    index: int
    type: NodeType
    solver: Solver

    # Geometry
    invert_elev: float
    max_depth: float
    surcharge_depth: float
    ponded_area: float
    initial_depth: float
    crown_elev: float
    full_volume: float
    degree: int

    # Hydraulic state
    depth: float
    head: float
    volume: float
    lateral_inflow: float
    overflow: float
    inflow: float
    losses: float
    outflow: float

    # Sub-views
    stats: NodeStatsView
    storage: StorageView
    outfall: OutfallView
    divider: DividerView

    def __init__(self, solver: Solver, index: int) -> None: ...
    def set_head_boundary(self, head: float) -> None: ...
    def quality(self, pollutant: _Key) -> float: ...
    def set_quality_mass_flux(self, pollutant: _Key, mass_rate: float) -> None: ...
    def depth_from_volume(self, volume: float) -> float: ...

    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __repr__(self) -> str: ...


class Nodes:
    """Indexable, iterable collection of :class:`Node` wrappers."""

    def __init__(self, solver: Solver) -> None: ...

    # Container protocol
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Node]: ...
    def __getitem__(self, key: _Key) -> Node: ...
    def __contains__(self, key: object) -> bool: ...

    # Identity lookups
    def get_index(self, node_id: str) -> int: ...
    def get_id(self, idx: int) -> str: ...

    # Editing (bumps the generation counter)
    def add(self, node_id: str, node_type: NodeType) -> Node: ...
    def pop_last(self, node_id: str) -> None: ...
    def rename(self, key: _Key, new_id: str) -> None: ...

    # Bulk numpy properties
    depths: NDArray[Any]
    heads: NDArray[Any]
    inflows: NDArray[Any]
    overflows: NDArray[Any]
    volumes: NDArray[Any]
    outflows: NDArray[Any]
    losses: NDArray[Any]
    lateral_inflows: NDArray[Any]
    ids: NDArray[Any]

    def set_lateral_inflows(self, values: NDArray[Any]) -> None: ...
    def qualities(self, pollutant: _Key) -> NDArray[Any]: ...

    def __repr__(self) -> str: ...
