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
    @property
    def node_max_depth(self) -> NDArray[Any]:
        """Maximum depth reached at each node, in project units."""
        ...
    @property
    def node_max_overflow(self) -> NDArray[Any]:
        """Maximum overflow (flooding) rate at each node, in project units."""
        ...
    @property
    def node_vol_flooded(self) -> NDArray[Any]:
        """Cumulative flooded volume at each node, in project units."""
        ...
    @property
    def node_time_flooded(self) -> NDArray[Any]:
        """Cumulative flooded duration at each node, in seconds."""
        ...

    # Link bulk
    @property
    def link_max_flow(self) -> NDArray[Any]:
        """Maximum flow rate in each link, in project units."""
        ...
    @property
    def link_max_velocity(self) -> NDArray[Any]:
        """Maximum flow velocity in each link, in project units."""
        ...
    @property
    def link_max_filling(self) -> NDArray[Any]:
        """Maximum filling ratio (depth / full depth) in each link."""
        ...
    @property
    def link_vol_flow(self) -> NDArray[Any]:
        """Cumulative flow volume through each link, in project units."""
        ...
    @property
    def link_surcharge_time(self) -> NDArray[Any]:
        """Cumulative surcharge duration in each link, in seconds."""
        ...

    # Subcatchment bulk
    @property
    def subcatchment_runoff_vol(self) -> NDArray[Any]:
        """Cumulative runoff volume from each subcatchment, in project units."""
        ...
    @property
    def subcatchment_max_runoff(self) -> NDArray[Any]:
        """Peak runoff rate from each subcatchment, in project units."""
        ...
    @property
    def subcatchment_precip(self) -> NDArray[Any]:
        """Cumulative precipitation depth per subcatchment, in project depth
        units (in for US, mm for SI). The only statistic without a C ``_bulk``
        companion; gathered scalar-wise in a ``nogil`` loop.
        """
        ...

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
