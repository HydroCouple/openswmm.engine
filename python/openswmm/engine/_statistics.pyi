"""
Simulation statistics (Pythonic v1 surface)
===========================================

Type stubs for :mod:`openswmm.engine._statistics`.
"""

import numpy as np

from ._solver import Solver


class Statistics:
    def __init__(self, solver: Solver) -> None: ...

    # Node bulk
    node_max_depth: np.ndarray
    node_max_overflow: np.ndarray
    node_vol_flooded: np.ndarray
    node_time_flooded: np.ndarray

    # Link bulk
    link_max_flow: np.ndarray
    link_max_velocity: np.ndarray
    link_max_filling: np.ndarray
    link_vol_flow: np.ndarray
    link_surcharge_time: np.ndarray

    # Subcatchment bulk
    subcatchment_runoff_vol: np.ndarray
    subcatchment_max_runoff: np.ndarray
