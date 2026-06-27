"""
Backward-compatibility shim that re-exports the full public API of
``openswmm.legacy.output`` under the shorter ``openswmm.output`` namespace.

.. deprecated::
    Import directly from ``openswmm.legacy.output`` instead::

        from openswmm.legacy.output import Output, FlowUnits

@note: Every name listed in the explicit re-export block below is guaranteed
    to remain importable from this module for backward-compatibility purposes,
    but new code should use the ``openswmm.legacy.output`` path directly.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT
"""
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
