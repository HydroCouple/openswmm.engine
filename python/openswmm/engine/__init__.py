"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

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
from ._solver import Solver, EngineError, run, run_with_callback

# =============================================================================
# Programmatic model building & editing
# =============================================================================
from ._model import ModelBuilder
from ._edit import ModelEditor, ImpactEntry, ConversionResult

# =============================================================================
# Domain object access (hydraulics)
# =============================================================================
from ._nodes import Nodes
from ._links import Links
from ._subcatchments import Subcatchments
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
from ._tables import Tables
from ._inflows import Inflows
from ._controls import Controls
from ._forcing import Forcing

# =============================================================================
# Spatial / infrastructure / 2D
# =============================================================================
from ._infrastructure import Infrastructure
from ._spatial import Spatial

# =============================================================================
# Optional extensions (require specific build flags)
# =============================================================================
try:
    from ._2d import Surface2D
    HAS_2D = True
except ImportError:
    HAS_2D = False

try:
    from ._geopackage import GeoPackage
    HAS_GEOPACKAGE = True
except ImportError:
    HAS_GEOPACKAGE = False

# =============================================================================
# Enumerations
# =============================================================================
from ._enums import (
    # Lifecycle / errors
    ErrorCode, EngineState, WarnCode, ObjectType,
    # Hydraulics
    FlowUnits, RouteModel, NodeType, LinkType, OutfallType, XSectShape,
    # Hydrology
    InfilModel, GageDataSource, GageRainType,
    # Water quality / LID
    ConcentrationUnits, BuildupFunc, WashoffFunc, LidType,
    # Output variables
    OutSubcatchVar, OutNodeVar, OutLinkVar, OutSystemVar,
    # Forcing & patterns
    ForcingMode, ForcingTarget, PatternType,
    # Mass-balance totals
    RunoffTotal, RoutingTotal,
)

__all__ = [
    # --- Engine lifecycle & errors ---
    "Solver", "EngineError", "run", "run_with_callback",
    # --- Programmatic model building & editing ---
    "ModelBuilder", "ModelEditor", "ImpactEntry", "ConversionResult",
    # --- Domain object access (hydraulics) ---
    "Nodes", "Links", "Subcatchments", "Gages",
    # --- Simulation state & I/O ---
    "HotStart", "MassBalance", "Statistics", "OutputReader",
    # --- Hydrology, water quality, and time-varying inputs ---
    "Pollutants", "Quality", "Tables", "Inflows", "Controls", "Forcing",
    # --- Spatial / infrastructure / 2D ---
    "Infrastructure", "Spatial",
    "Surface2D", "HAS_2D",
    # --- Optional extensions ---
    "HAS_GEOPACKAGE",
    # --- Enumerations: lifecycle / errors ---
    "ErrorCode", "EngineState", "WarnCode", "ObjectType",
    # --- Enumerations: hydraulics ---
    "FlowUnits", "RouteModel", "NodeType", "LinkType",
    "OutfallType", "XSectShape",
    # --- Enumerations: hydrology ---
    "InfilModel", "GageDataSource", "GageRainType",
    # --- Enumerations: water quality / LID ---
    "ConcentrationUnits", "BuildupFunc", "WashoffFunc", "LidType",
    # --- Enumerations: output variables ---
    "OutSubcatchVar", "OutNodeVar", "OutLinkVar", "OutSystemVar",
    # --- Enumerations: forcing & patterns ---
    "ForcingMode", "ForcingTarget", "PatternType",
    # --- Enumerations: mass-balance totals ---
    "RunoffTotal", "RoutingTotal",
]
