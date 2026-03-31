# Backward-compatibility shim: openswmm.output -> openswmm.legacy.output
# Deprecated: use ``from openswmm.legacy.output import ...`` instead.
from openswmm.legacy.output import *  # noqa: F401,F403
from openswmm.legacy.output import (  # noqa: F401  explicit re-exports
    UnitSystem,
    FlowUnits,
    ConcentrationUnits,
    ElementType,
    TimeAttribute,
    SubcatchAttribute,
    NodeAttribute,
    LinkAttribute,
    SystemAttribute,
    SWMMOutputException,
    Output,
)
