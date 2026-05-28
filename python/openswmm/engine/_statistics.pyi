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
