"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for the :mod:`openswmm.engine` package.

Cython bindings for the OpenSWMM Engine 6.0 C API, providing classes for
engine lifecycle management, programmatic model building, domain object
access, hot start file management, mass balance queries, runtime forcing,
water quality, spatial data, and GeoPackage I/O.
"""

from ._2d import Surface2D as Surface2D
from ._controls import Controls as Controls
from ._edit import (
    ConversionResult as ConversionResult,
    ImpactEntry as ImpactEntry,
    ModelEditor as ModelEditor,
)
from ._enums import (
    BuildupFunc as BuildupFunc,
    ConcentrationUnits as ConcentrationUnits,
    EngineState as EngineState,
    ErrorCode as ErrorCode,
    FlowUnits as FlowUnits,
    ForcingMode as ForcingMode,
    ForcingTarget as ForcingTarget,
    GageDataSource as GageDataSource,
    GageRainType as GageRainType,
    InfilModel as InfilModel,
    LidType as LidType,
    LinkType as LinkType,
    NodeType as NodeType,
    ObjectType as ObjectType,
    OutfallType as OutfallType,
    OutLinkVar as OutLinkVar,
    OutNodeVar as OutNodeVar,
    OutSubcatchVar as OutSubcatchVar,
    OutSystemVar as OutSystemVar,
    PatternType as PatternType,
    RouteModel as RouteModel,
    RoutingTotal as RoutingTotal,
    RunoffTotal as RunoffTotal,
    WarnCode as WarnCode,
    WashoffFunc as WashoffFunc,
    XSectShape as XSectShape,
)
from ._forcing import Forcing as Forcing
from ._gages import Gages as Gages
from ._geopackage import GeoPackage as GeoPackage
from ._hotstart import HotStart as HotStart
from ._inflows import Inflows as Inflows
from ._infrastructure import Infrastructure as Infrastructure
from ._links import Links as Links
from ._massbalance import MassBalance as MassBalance
from ._model import ModelBuilder as ModelBuilder
from ._nodes import Nodes as Nodes
from ._output_reader import OutputReader as OutputReader
from ._pollutants import Pollutants as Pollutants
from ._quality import Quality as Quality
from ._solver import EngineError as EngineError, Solver as Solver
from ._spatial import Spatial as Spatial
from ._statistics import Statistics as Statistics
from ._subcatchments import Subcatchments as Subcatchments
from ._tables import Tables as Tables

HAS_GEOPACKAGE: bool

__all__: list[str]
