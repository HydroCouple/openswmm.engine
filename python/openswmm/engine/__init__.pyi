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
    Solver as Solver,
    run as run,
    run_with_callback as run_with_callback,
)
from ._exceptions import (
    BadHandleError as BadHandleError,
    BadIndexError as BadIndexError,
    BadParamError as BadParamError,
    CRSError as CRSError,
    DependencyError as DependencyError,
    EngineError as EngineError,
    FileError as FileError,
    HotStartError as HotStartError,
    LifecycleError as LifecycleError,
    NumericalError as NumericalError,
    ParseError as ParseError,
    PluginError as PluginError,
    StaleObjectError as StaleObjectError,
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
from ._geometry import CrossSection as CrossSection
from ._xsect import (
    XSectionGeometry as XSectionGeometry,
    shape_name as shape_name,
)
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
# DateTime conversion (SWMM DateTime double <-> Python datetime / calendar parts)
# =============================================================================
from . import _datetime as datetime_api
from ._dates import (
    datetime_to_oadate as datetime_to_oadate,
    oadate_to_datetime as oadate_to_datetime,
)

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
    OrificeType as OrificeType,
    OutletRatingType as OutletRatingType,
    RouteModel as RouteModel,
    WeirType as WeirType,
    XSectShape as XSectShape,
    # Hydrology
    GageDataSource as GageDataSource,
    GageRainType as GageRainType,
    InfilModel as InfilModel,
    # Water quality / LID
    AquiferParam as AquiferParam,
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
    ForcingType as ForcingType,
    ForcingPersist as ForcingPersist,
    PatternType as PatternType,
    # 2D surface routing
    SurfaceForcingMode as SurfaceForcingMode,
    SurfaceBoundaryType as SurfaceBoundaryType,
    # Nodes / editing
    DividerType as DividerType,
    RefType as RefType,
    # Mass-balance totals
    RoutingTotal as RoutingTotal,
    RunoffTotal as RunoffTotal,
)

HAS_GEOPACKAGE: bool

__all__: list[str]
