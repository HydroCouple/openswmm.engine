"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for the :mod:`openswmm.engine` package.

Cython bindings for the OpenSWMM Engine 6.0 C API, providing classes for
engine lifecycle management, programmatic model building, domain object
access, hot start file management, and mass balance queries.
"""

from ._enums import (
    ErrorCode as ErrorCode,
    EngineState as EngineState,
    FlowUnits as FlowUnits,
    LinkType as LinkType,
    NodeType as NodeType,
    RouteModel as RouteModel,
    XSectShape as XSectShape,
)
from ._gages import Gages as Gages
from ._hotstart import HotStart as HotStart
from ._links import Links as Links
from ._massbalance import MassBalance as MassBalance
from ._model import ModelBuilder as ModelBuilder
from ._nodes import Nodes as Nodes
from ._solver import EngineError as EngineError, Solver as Solver
from ._subcatchments import Subcatchments as Subcatchments

__all__: list[str]
