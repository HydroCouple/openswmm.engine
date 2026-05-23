"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for the L{openswmm.engine} package.

Cython bindings for the OpenSWMM Engine 6.0 C API, providing classes for
engine lifecycle management, programmatic model building, domain object
access, hot start file management, mass balance queries, runtime forcing,
water quality, spatial data, and GeoPackage I/O.
"""

# =============================================================================
# Engine lifecycle & errors
# =============================================================================
from ._solver import (
    EngineError as EngineError,
    Solver as Solver,
)

# =============================================================================
# Programmatic model building & editing
# =============================================================================
from ._model import ModelBuilder as ModelBuilder
from ._edit import (
    ConversionResult as ConversionResult,
    ImpactEntry as ImpactEntry,
    ModelEditor as ModelEditor,
)

# =============================================================================
# Domain object access (hydraulics)
# =============================================================================
from ._nodes import Nodes as Nodes
from ._links import Links as Links
from ._subcatchments import Subcatchments as Subcatchments
from ._gages import Gages as Gages

# =============================================================================
# Simulation state & I/O
# =============================================================================
from ._hotstart import HotStart as HotStart
from ._massbalance import MassBalance as MassBalance
from ._statistics import Statistics as Statistics
from ._output_reader import OutputReader as OutputReader

# =============================================================================
# Hydrology, water quality, and time-varying inputs
# =============================================================================
from ._pollutants import Pollutants as Pollutants
from ._quality import Quality as Quality
from ._tables import Tables as Tables
from ._inflows import Inflows as Inflows
from ._controls import Controls as Controls
from ._forcing import Forcing as Forcing

# =============================================================================
# Spatial / infrastructure / 2D
# =============================================================================
from ._infrastructure import Infrastructure as Infrastructure
from ._spatial import Spatial as Spatial
from ._2d import Surface2D as Surface2D

# =============================================================================
# Optional GeoPackage I/O (only available with OPENSWMM_WITH_GEOPACKAGE build)
# =============================================================================
from ._geopackage import GeoPackage as GeoPackage

# =============================================================================
# Enumerations
# =============================================================================
from ._enums import (
    # Lifecycle / errors
    EngineState as EngineState,
    ErrorCode as ErrorCode,
    ObjectType as ObjectType,
    WarnCode as WarnCode,
    # Hydraulics
    FlowUnits as FlowUnits,
    LinkType as LinkType,
    NodeType as NodeType,
    OutfallType as OutfallType,
    RouteModel as RouteModel,
    XSectShape as XSectShape,
    # Hydrology
    GageDataSource as GageDataSource,
    GageRainType as GageRainType,
    InfilModel as InfilModel,
    # Water quality / LID
    BuildupFunc as BuildupFunc,
    ConcentrationUnits as ConcentrationUnits,
    LidType as LidType,
    WashoffFunc as WashoffFunc,
    # Output variables
    OutLinkVar as OutLinkVar,
    OutNodeVar as OutNodeVar,
    OutSubcatchVar as OutSubcatchVar,
    OutSystemVar as OutSystemVar,
    # Forcing & patterns
    ForcingMode as ForcingMode,
    ForcingTarget as ForcingTarget,
    PatternType as PatternType,
    # Mass-balance totals
    RoutingTotal as RoutingTotal,
    RunoffTotal as RunoffTotal,
)

HAS_GEOPACKAGE: bool

__all__: list[str]
