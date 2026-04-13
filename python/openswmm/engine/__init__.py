"""
openswmm.engine
===============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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
   * - :class:`Pollutants`
     - ``openswmm_pollutants.h``
     - Pollutant management and quality injection
   * - :class:`Tables`
     - ``openswmm_tables.h``
     - Time series, curves, patterns
   * - :class:`Inflows`
     - ``openswmm_inflows.h``
     - External inflows, DWF, RDII
   * - :class:`Controls`
     - ``openswmm_controls.h``
     - Control rules and direct actions
   * - :class:`Infrastructure`
     - ``openswmm_infrastructure.h``
     - Transects, streets, inlets, LIDs
   * - :class:`Quality`
     - ``openswmm_quality.h``
     - Landuse, buildup, washoff, treatment
   * - :class:`Statistics`
     - ``openswmm_statistics.h``
     - Cumulative simulation statistics
   * - :class:`OutputReader`
     - ``openswmm_output.h``
     - Binary output file reader
   * - :class:`Spatial`
     - ``openswmm_spatial.h``
     - Coordinates, CRS, vertices, polygons
   * - :class:`Forcing`
     - ``openswmm_forcing.h``
     - Advanced runtime forcing (mode + persistence)

Quick start
-----------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)
        while s.step():
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
    while solver.step():
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

        while s.step():
            pass

        forcing.clear_all()
"""

# Lifecycle and error
from ._solver import Solver, EngineError

# Model building
from ._model import ModelBuilder

# Domain access classes
from ._nodes import Nodes
from ._links import Links
from ._subcatchments import Subcatchments
from ._gages import Gages

# Hot start
from ._hotstart import HotStart

# Mass balance
from ._massbalance import MassBalance

# Domain modules
from ._pollutants import Pollutants
from ._tables import Tables
from ._inflows import Inflows
from ._controls import Controls
from ._infrastructure import Infrastructure
from ._quality import Quality
from ._statistics import Statistics
from ._output_reader import OutputReader
from ._spatial import Spatial
from ._forcing import Forcing

# GeoPackage (optional — requires OPENSWMM_WITH_GEOPACKAGE build)
try:
    from ._geopackage import GeoPackage
    HAS_GEOPACKAGE = True
except ImportError:
    HAS_GEOPACKAGE = False

# Enums (pure Python, always available)
from ._enums import (
    ErrorCode, EngineState, NodeType, LinkType,
    XSectShape, FlowUnits, RouteModel,
    GageDataSource, GageRainType, InfilModel, OutfallType,
    ConcentrationUnits, BuildupFunc, WashoffFunc,
    RunoffTotal, RoutingTotal, LidType, PatternType,
    ForcingMode, ForcingTarget,
    OutSubcatchVar, OutNodeVar, OutLinkVar, OutSystemVar,
)

__all__ = [
    # Classes
    "Solver", "ModelBuilder", "HotStart", "EngineError",
    "Nodes", "Links", "Subcatchments", "Gages", "MassBalance",
    "Pollutants", "Tables", "Inflows", "Controls",
    "Infrastructure", "Quality", "Statistics", "OutputReader",
    "Spatial", "Forcing", "HAS_GEOPACKAGE",
    # Enums
    "ErrorCode", "EngineState", "NodeType", "LinkType",
    "XSectShape", "FlowUnits", "RouteModel",
    "GageDataSource", "GageRainType", "InfilModel", "OutfallType",
    "ConcentrationUnits", "BuildupFunc", "WashoffFunc",
    "RunoffTotal", "RoutingTotal", "LidType", "PatternType",
    "ForcingMode", "ForcingTarget",
    "OutSubcatchVar", "OutNodeVar", "OutLinkVar", "OutSystemVar",
]
