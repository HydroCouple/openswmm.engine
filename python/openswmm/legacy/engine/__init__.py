"""
openswmm.legacy.engine
======================

Legacy EPA SWMM 5.x solver bindings (Cython).

This subpackage provides the :class:`Solver` class for running SWMM
simulations, enumeration types for object/property access, and utility
functions for date encoding and version queries.
"""
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

try:
    from ._solver import (
        SWMMObjects,
        SWMMNodeTypes,
        SWMMLinkTypes,
        SWMMRainGageProperties,
        SWMMSubcatchmentProperties,
        SWMMNodeProperties,
        SWMMLinkProperties,
        SWMMSystemProperties,
        SWMMFlowUnits,
        SWMMAPIErrors,
        run_solver,
        decode_swmm_datetime,
        encode_swmm_datetime,
        version,
        get_error_message,
        SolverState,
        CallbackType,
        SWMMSolverException,
        Solver,
    )
except ImportError:
    pass

# Pure-Python OOP wrappers (Phase 3+4) — always importable
from ._nodes import LegacyNodes, LegacyNode
from ._links import LegacyLinks, LegacyLink
from ._subcatchments import LegacySubcatchments, LegacySubcatchment
from ._raingages import LegacyRainGages, LegacyRainGage
from ._system import LegacySystem
from ._forcing_log import ExternalForcingLog
