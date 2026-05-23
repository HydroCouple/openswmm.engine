"""
Backward-compatibility shim that re-exports the full public API of
``openswmm.legacy.engine`` under the shorter ``openswmm.solver`` namespace.

.. deprecated::
    Import directly from ``openswmm.legacy.engine`` instead::

        from openswmm.legacy.engine import Solver, run_solver

@note: Every name listed in the explicit re-export block below is guaranteed
    to remain importable from this module for backward-compatibility purposes,
    but new code should use the ``openswmm.legacy.engine`` path directly.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT
"""
# Backward-compatibility shim: openswmm.solver -> openswmm.legacy.engine
# Deprecated: use ``from openswmm.legacy.engine import ...`` instead.
from openswmm.legacy.engine import *  # noqa: F401,F403
from openswmm.legacy.engine import (  # noqa: F401  explicit re-exports
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
