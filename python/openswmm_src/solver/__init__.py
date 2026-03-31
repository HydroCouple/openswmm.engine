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
