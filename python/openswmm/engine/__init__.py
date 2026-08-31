# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Cython bindings for the OpenSWMM Engine 6.0 C API.

The package is split by domain to mirror the C header organisation:

.. list-table:: Module Map
   :header-rows: 1

   * - Python class
     - C header
     - Contents
   * - :class:`Solver`
     - ``openswmm_engine.h``
     - Engine lifecycle, timing, error reporting
   * - :class:`ModelBuilder`
     - ``openswmm_model.h``
     - Programmatic model construction
   * - :class:`ModelEditor`
     - ``openswmm_edit.h``
     - In-place model editing (delete + type conversion)
   * - :class:`Nodes`
     - ``openswmm_nodes.h``
     - Node get/set, lateral inflow, bulk arrays
   * - :class:`Links`
     - ``openswmm_links.h``
     - Link get/set, control settings, bulk arrays
   * - :class:`XSectionGeometry`
     - ``openswmm_xsect.h``
     - Cross-section geometry kernels (standalone or per-link)
   * - :class:`Subcatchments`
     - ``openswmm_subcatchments.h``
     - Subcatchment runoff, rainfall override
   * - :class:`Gages`
     - ``openswmm_gages.h``
     - Rain gage rainfall get/set
   * - :class:`HotStart`
     - ``openswmm_hotstart.h``
     - Hot start save/open/apply/close
   * - :class:`MassBalance`
     - ``openswmm_massbalance.h``
     - Continuity error queries
   * - :class:`Statistics`
     - ``openswmm_statistics.h``
     - Cumulative simulation statistics
   * - :class:`OutputReader`
     - ``openswmm_output.h``
     - Binary output file reader
   * - :class:`Pollutants`
     - ``openswmm_pollutants.h``
     - Pollutant management and quality injection
   * - :class:`Quality`
     - ``openswmm_quality.h``
     - Landuse, buildup, washoff, treatment
   * - :class:`InitialQuality`
     - ``openswmm_initial_quality.h``
     - ``[INITIAL_QUALITY]`` per-element starting concentrations
   * - :class:`Reactions`
     - ``openswmm_reactions.h``
     - Multi-species reaction system: species, coefficients, terms, expressions
   * - :class:`Heat`
     - ``openswmm_heat.h``
     - Heat-transport fluxes, solar/cloud forcing, inlet temperatures
   * - :class:`WaterAge`
     - ``openswmm_water_age.h``
     - ``[WATER_AGE_SOURCES]`` per-pathway source ages
   * - :class:`ProcessComponents`
     - ``openswmm_process_components.h``
     - ``[PROCESS_COMPONENTS]`` registrations and config-file bindings
   * - :class:`Tables`
     - ``openswmm_tables.h``
     - Time series, curves, patterns
   * - :class:`Inflows`
     - ``openswmm_inflows.h``
     - External inflows, DWF, RDII
   * - :class:`Controls`
     - ``openswmm_controls.h``
     - Control rules and direct actions
   * - :class:`Forcing`
     - ``openswmm_forcing.h``
     - Advanced runtime forcing (mode + persistence)
   * - :class:`Infrastructure`
     - ``openswmm_infrastructure.h``
     - Transects, streets, inlets, LIDs
   * - :class:`Spatial`
     - ``openswmm_spatial.h``
     - Coordinates, CRS, vertices, polygons
   * - :class:`Surface2D`
     - ``openswmm_2d.h``
     - 2D surface mesh / coupled overland flow
   * - :class:`GeoPackage` (Optional)
     - ``openswmm_geopackage.h``
     - GeoPackage import/export (requires ``OPENSWMM_WITH_GEOPACKAGE`` build)

Quick start
-----------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            depths = nodes.get_depths_bulk()  # numpy array
            flows  = links.get_flows_bulk()   # numpy array

Programmatic model building
----------------------------

.. code-block:: python

    from openswmm.engine import ModelBuilder, NodeType, LinkType, XSectShape

    m = ModelBuilder()
    m.add_node("J1", NodeType.JUNCTION)
    m.add_node("OUT1", NodeType.OUTFALL)
    m.add_link("C1", LinkType.CONDUIT)
    m.set_link_nodes(0, 0, 1)
    m.set_link_length(0, 300.0)
    m.set_link_roughness(0, 0.013)
    m.set_link_xsect(0, XSectShape.CIRCULAR, 1.0)
    m.validate()
    m.finalize()

    solver = m.to_solver()
    solver.start()
    while solver.state == EngineState.RUNNING:
        if solver.step() != 0:
            break
        pass
    solver.end()
    solver.destroy()

Advanced forcing
----------------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Forcing, ForcingMode

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes   = Nodes(s)
        forcing = Forcing(s)

        j1 = nodes.get_index("J1")
        forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)

        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass

        forcing.clear_all()
"""

# =============================================================================
# Engine lifecycle & errors
# =============================================================================
from ._solver import Solver, run, run_with_callback, GEOPACKAGE_PLUGIN_ID
from ._exceptions import (
    EngineError,
    BadHandleError,
    BadIndexError,
    BadParamError,
    LifecycleError,
    HotStartError,
    PluginError,
    FileError,
    ParseError,
    NumericalError,
    CRSError,
    DependencyError,
    StaleObjectError,
)

# =============================================================================
# Programmatic model building & editing
# =============================================================================
from ._model import ModelBuilder
from ._edit import ModelEditor, ImpactEntry, ConversionResult
from ._geometry import CrossSection
from ._xsect import XSectionGeometry, shape_name
from ._report import (
    get_report_snapshot,
    ReportSnapshot,
    RoutingDiagnostics,
    RunoffContinuity,
    RoutingContinuity,
    QualityContinuity,
    NodeFloodingEntry,
    LinkFlowEntry,
    PumpEntry,
    SubcatchmentEntry,
)

# =============================================================================
# Domain object access (hydraulics)
# =============================================================================
from ._nodes import Nodes
from ._links import Links
from ._subcatchments import Subcatchments, Aquifers, Snowpacks, GroundwaterParams
from ._gages import Gages

# =============================================================================
# Simulation state & I/O
# =============================================================================
from ._hotstart import HotStart
from ._massbalance import MassBalance
from ._statistics import Statistics
from ._output_reader import OutputReader

# =============================================================================
# Hydrology, water quality, and time-varying inputs
# =============================================================================
from ._pollutants import Pollutants
from ._quality import Quality
from ._initial_quality import InitialQuality, InitialQualityEntry
from ._tables import Tables, Patterns
from ._inflows import Inflows
from ._controls import Controls
from ._forcing import Forcing
from ._climate import Climate

# =============================================================================
# Transport processes — heat, water age, reactions, process components
# =============================================================================
from ._heat import Heat, HeatNodeOverride
from ._water_age import WaterAge, WaterAgeOverride
from ._reactions import (
    Reactions,
    ReactionSpecies,
    ReactionCoefficient,
    ReactionTerm,
    ReactionInitialEntry,
    ReactionHydVar,
    ReactionFunction,
    ExpressionDiagnostic,
)
from ._process_components import ProcessComponents, ProcessComponent

# =============================================================================
# Spatial / infrastructure / 2D
# =============================================================================
from ._infrastructure import Infrastructure
from ._spatial import Spatial

# =============================================================================
# Optional extensions (require specific build flags)
# =============================================================================
try:
    from ._2d import (Surface2D, Infiltration2DView, Infil2DDefaults,
                      Infil2DRow, Infil2DCell)
    HAS_2D = True
except ImportError:
    HAS_2D = False

try:
    from ._geopackage import GeoPackage
    HAS_GEOPACKAGE = True
except ImportError:
    HAS_GEOPACKAGE = False

# =============================================================================
# DateTime conversion (SWMM DateTime double <-> Python datetime / calendar parts)
# =============================================================================
from ._dates import oadate_to_datetime, datetime_to_oadate
from . import _datetime as datetime_api  # low-level C API wrappers

# =============================================================================
# Enumerations
# =============================================================================
from ._enums import (
    # Lifecycle / errors
    ErrorCode, EngineState, WarnCode, ObjectType,
    # Hydraulics
    FlowUnits, RouteModel, NodeType, LinkType, OutfallType, StorageShape,
    XSectShape, OrificeType, WeirType, OutletRatingType,
    # Hydrology
    InfilModel, GageDataSource, GageRainType,
    # Water quality / LID
    ConcentrationUnits, BuildupFunc, WashoffFunc, LidType,
    # Hydrology parameters
    AquiferParam,
    # Output variables
    OutSubcatchVar, OutNodeVar, OutLinkVar, OutSystemVar,
    # Forcing & patterns
    ForcingMode, ForcingTarget, ForcingType, ForcingPersist, PatternType,
    # 2D surface routing
    SurfaceForcingMode, SurfaceBoundaryType,
    SurfaceInfilMethod, SurfaceInfilDest,
    # Nodes / editing
    DividerType, RefType,
    # Tables / model files
    TableType, FilePathRole, UserFlagType,
    # Transport processes — heat, water age, reactions
    HeatFluxModule, HeatShortwaveMode, HeatRadiativeParam, HeatSolarParam,
    HeatCloudParam, HeatSourceKind, WaterAgeSource,
    ReactionScope, ReactionExprForm,
    # Mass-balance totals
    RunoffTotal, RoutingTotal,
)

__all__ = [
    # --- DateTime conversion (SWMM DateTime <-> Python datetime) ---
    "oadate_to_datetime", "datetime_to_oadate", "datetime_api",
    # --- Engine lifecycle & errors ---
    "Solver", "run", "run_with_callback", "GEOPACKAGE_PLUGIN_ID",
    "EngineError",
    "BadHandleError", "BadIndexError", "BadParamError",
    "LifecycleError", "HotStartError", "PluginError",
    "FileError", "ParseError", "NumericalError",
    "CRSError", "DependencyError", "StaleObjectError",
    # --- Programmatic model building & editing ---
    "ModelBuilder", "ModelEditor", "ImpactEntry", "ConversionResult",
    # --- Geometry helpers ---
    "CrossSection",
    "XSectionGeometry",
    "shape_name",
    # --- Report snapshot ---
    "get_report_snapshot",
    "ReportSnapshot", "RoutingDiagnostics",
    "RunoffContinuity", "RoutingContinuity", "QualityContinuity",
    "NodeFloodingEntry", "LinkFlowEntry", "PumpEntry", "SubcatchmentEntry",
    # --- Domain object access (hydraulics) ---
    "Nodes", "Links", "Subcatchments", "Gages",
    "Aquifers", "Snowpacks",
    # --- Simulation state & I/O ---
    "HotStart", "MassBalance", "Statistics", "OutputReader",
    # --- Hydrology, water quality, and time-varying inputs ---
    "Pollutants", "Quality", "Tables", "Patterns", "Inflows", "Controls", "Forcing",
    "Climate", "InitialQuality", "InitialQualityEntry",
    # --- Transport processes — heat, water age, reactions ---
    "Heat", "HeatNodeOverride",
    "WaterAge", "WaterAgeOverride",
    "Reactions", "ReactionSpecies", "ReactionCoefficient", "ReactionTerm",
    "ReactionInitialEntry", "ReactionHydVar", "ReactionFunction",
    "ExpressionDiagnostic",
    "ProcessComponents", "ProcessComponent",
    # --- Spatial / infrastructure / 2D ---
    "Infrastructure", "Spatial",
    "Surface2D", "HAS_2D",
    "Infiltration2DView", "Infil2DDefaults", "Infil2DRow", "Infil2DCell",
    # --- Optional extensions ---
    "HAS_GEOPACKAGE",
    # --- Enumerations: lifecycle / errors ---
    "ErrorCode", "EngineState", "WarnCode", "ObjectType",
    # --- Enumerations: hydraulics ---
    "FlowUnits", "RouteModel", "NodeType", "LinkType",
    "OutfallType", "StorageShape", "XSectShape",
    "OrificeType", "WeirType", "OutletRatingType",
    # --- Enumerations: hydrology ---
    "InfilModel", "GageDataSource", "GageRainType",
    # --- Enumerations: water quality / LID ---
    "ConcentrationUnits", "BuildupFunc", "WashoffFunc", "LidType",
    # --- Enumerations: hydrology parameters ---
    "AquiferParam",
    # --- Enumerations: output variables ---
    "OutSubcatchVar", "OutNodeVar", "OutLinkVar", "OutSystemVar",
    # --- Enumerations: forcing & patterns ---
    "ForcingMode", "ForcingTarget", "ForcingType", "ForcingPersist", "PatternType",
    # --- Enumerations: 2D surface routing ---
    "SurfaceForcingMode", "SurfaceBoundaryType",
    "SurfaceInfilMethod", "SurfaceInfilDest",
    "DividerType", "RefType",
    "TableType", "FilePathRole", "UserFlagType",
    # --- Enumerations: transport processes ---
    "HeatFluxModule", "HeatShortwaveMode", "HeatRadiativeParam", "HeatSolarParam",
    "HeatCloudParam", "HeatSourceKind", "WaterAgeSource",
    "ReactionScope", "ReactionExprForm",
    # --- Enumerations: mass-balance totals ---
    "RunoffTotal", "RoutingTotal",
]
