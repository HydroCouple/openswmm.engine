"""
Link access (Pythonic v1 surface)
=================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._links`.
"""

from collections.abc import Iterator
from typing import Any, Tuple, Union

import numpy as np

from ._enums import LinkType, OrificeType, OutletRatingType, WeirType, XSectShape
from ._nodes import Node
from ._solver import Solver


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
    def __iter__(self) -> Iterator: ...


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

    # Cross-section
    xsect: XSection

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

    # Editing (bumps generation)
    def add(self, link_id: str, link_type: LinkType) -> Link: ...
    def pop_last(self, link_id: str) -> None: ...
    def rename(self, key: _Key, new_id: str) -> None: ...

    # Bulk numpy properties
    flows: np.ndarray
    depths: np.ndarray
    velocities: np.ndarray
    capacities: np.ndarray
    volumes: np.ndarray
    control_settings: np.ndarray
    target_settings: np.ndarray
    hyd_powers: np.ndarray
    ids: np.ndarray

    def qualities(self, pollutant: _Key) -> np.ndarray: ...
    def pump_stats(self) -> Tuple[np.ndarray, np.ndarray, np.ndarray]: ...

    def __repr__(self) -> str: ...
