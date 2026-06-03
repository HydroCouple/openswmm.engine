"""
Simulation statistics (Pythonic v1 surface)
===========================================

Type stubs for :mod:`openswmm.engine._statistics`.
"""

from typing import Any

import numpy as np
from numpy.typing import NDArray

from ._solver import Solver


class Statistics:
    def __init__(self, solver: Solver) -> None: ...

    # Node bulk
    node_max_depth: NDArray[Any]
    node_max_overflow: NDArray[Any]
    node_vol_flooded: NDArray[Any]
    node_time_flooded: NDArray[Any]

    # Link bulk
    link_max_flow: NDArray[Any]
    link_max_velocity: NDArray[Any]
    link_max_filling: NDArray[Any]
    link_vol_flow: NDArray[Any]
    link_surcharge_time: NDArray[Any]

    # Subcatchment bulk
    subcatchment_runoff_vol: NDArray[Any]
    subcatchment_max_runoff: NDArray[Any]
    subcatchment_precip: NDArray[Any]
    """Cumulative precipitation depth per subcatchment, in project depth
    units (in for US, mm for SI). The only statistic without a C ``_bulk``
    companion; gathered scalar-wise in a ``nogil`` loop.
    """

    # Scalar per-element getters (P2.6) — single-index reads without the
    # whole-network array allocation. All values in project units.
    def node_max_depth_at(self, idx: int) -> float: ...
    def node_max_overflow_at(self, idx: int) -> float: ...
    def node_vol_flooded_at(self, idx: int) -> float: ...
    def node_time_flooded_at(self, idx: int) -> float: ...
    def link_max_flow_at(self, idx: int) -> float: ...
    def link_max_velocity_at(self, idx: int) -> float: ...
    def link_max_filling_at(self, idx: int) -> float: ...
    def link_surcharge_time_at(self, idx: int) -> float: ...
    def link_vol_flow_at(self, idx: int) -> float: ...
    def subcatchment_max_runoff_at(self, idx: int) -> float: ...
    def subcatchment_runoff_vol_at(self, idx: int) -> float: ...
