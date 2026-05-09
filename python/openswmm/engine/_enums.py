"""
Enumerations
============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Integer-backed enums mirroring the C API enum definitions in
``openswmm_engine.h``. These are pure Python (no Cython required)
and can be used for type-safe comparisons:

.. code-block:: python

    from openswmm.engine import NodeType, EngineState

    if solver.state == EngineState.RUNNING:
        ...
"""

from enum import IntEnum


class ErrorCode(IntEnum):
    """SWMM C API return codes.

    :cvar OK: Success (0).
    :cvar NOMEM: Out of memory.
    :cvar LIFECYCLE: Function called in wrong lifecycle state.
    :cvar BADHANDLE: NULL or invalid engine handle.
    :cvar BADINDEX: Object index out of range.
    """

    OK = 0
    NOMEM = 1
    INPFILE = 2
    RPTFILE = 3
    OUTFILE = 4
    PARSE = 5
    LIFECYCLE = 6
    BADHANDLE = 7
    BADINDEX = 8
    BADPARAM = 9
    PLUGIN = 10
    IO = 11
    HOTSTART = 12
    CRS = 13
    NUMERICAL = 14
    INTERNAL = 99


class EngineState(IntEnum):
    NONE = 0
    CREATED = 1
    OPENED = 2
    INITIALIZED = 3
    STARTED = 4
    RUNNING = 5
    ENDED = 6
    CLOSED = 7
    BUILDING = 8


class NodeType(IntEnum):
    JUNCTION = 0
    OUTFALL = 1
    STORAGE = 2
    DIVIDER = 3


class LinkType(IntEnum):
    CONDUIT = 0
    PUMP = 1
    ORIFICE = 2
    WEIR = 3
    OUTLET = 4


class XSectShape(IntEnum):
    CIRCULAR = 0
    FILLED_CIRCULAR = 1
    RECT_CLOSED = 2
    RECT_OPEN = 3
    TRAPEZOIDAL = 4
    TRIANGULAR = 5
    PARABOLIC = 6
    POWER = 7
    MODBASKETHANDLE = 8
    EGGSHAPED = 9
    HORSESHOE = 10
    GOTHIC = 11
    CATENARY = 12
    SEMIELLIPTICAL = 13
    BASKETHANDLE = 14
    SEMICIRCULAR = 15
    IRREGULAR = 16
    CUSTOM = 17
    FORCE_MAIN = 18


class FlowUnits(IntEnum):
    CFS = 0
    GPM = 1
    MGD = 2
    CMS = 3
    LPS = 4
    MLD = 5


class RouteModel(IntEnum):
    STEADY = 0
    KINWAVE = 1
    DYNWAVE = 2


class GageDataSource(IntEnum):
    """Rain gage data source type."""
    TIMESERIES = 0
    FILE = 1


class GageRainType(IntEnum):
    """Rain gage rainfall data format."""
    INTENSITY = 0
    VOLUME = 1
    CUMULATIVE = 2


class InfilModel(IntEnum):
    """Infiltration model type."""
    HORTON = 0
    MOD_HORTON = 1
    GREEN_AMPT = 2
    MOD_GREEN_AMPT = 3
    CURVE_NUMBER = 4


class OutfallType(IntEnum):
    """Outfall boundary condition type."""
    FREE = 0
    NORMAL = 1
    FIXED = 2
    TIDAL = 3
    TIMESERIES = 4


class ConcentrationUnits(IntEnum):
    """Pollutant concentration units."""
    MG_PER_L = 0
    UG_PER_L = 1
    COUNT_PER_L = 2


class BuildupFunc(IntEnum):
    """Pollutant buildup function type."""
    NONE = 0
    POW = 1
    EXP = 2
    SAT = 3
    EXT = 4


class WashoffFunc(IntEnum):
    """Pollutant washoff function type."""
    NONE = 0
    EXP = 1
    RC = 2
    EMC = 3


class RunoffTotal(IntEnum):
    """Runoff mass balance component codes."""
    RAINFALL = 0
    EVAP = 1
    INFIL = 2
    RUNOFF = 3
    SNOWREMOV = 4
    INITSTORE = 5
    FINALSTORE = 6


class RoutingTotal(IntEnum):
    """Routing mass balance component codes."""
    DRY_WEATHER = 0
    WET_WEATHER = 1
    GW_INFLOW = 2
    RDII = 3
    EXTERNAL = 4
    FLOODING = 5
    OUTFLOW = 6
    EVAP_LOSS = 7
    SEEP_LOSS = 8
    INIT_STORAGE = 9
    FINAL_STORAGE = 10


class LidType(IntEnum):
    """LID (Low Impact Development) control type."""
    BIO_CELL = 0
    RAIN_GARDEN = 1
    GREEN_ROOF = 2
    INFIL_TRENCH = 3
    PERM_PAVEMENT = 4
    RAIN_BARREL = 5
    ROOFTOP_DISCONN = 6
    VEGETATIVE_SWALE = 7


class PatternType(IntEnum):
    """Time pattern type."""
    MONTHLY = 0
    DAILY = 1
    HOURLY = 2
    WEEKEND = 3


class ForcingMode(IntEnum):
    """Forcing application mode.

    Determines how a forced value is combined with the model-computed value.

    :cvar REPLACE: Replace the computed value entirely.
    :cvar ADD: Add the forced value to the computed value.
    """
    REPLACE = 0
    ADD = 1


class ForcingTarget(IntEnum):
    """Object type codes used with :meth:`Forcing.clear`.

    :cvar NODE: Node forcing.
    :cvar LINK: Link forcing.
    :cvar SUBCATCH: Subcatchment forcing.
    :cvar GAGE: Rain gage forcing.
    """
    NODE = 0
    LINK = 1
    SUBCATCH = 2
    GAGE = 3


class OutSubcatchVar(IntEnum):
    """Subcatchment output result variable indices."""
    RAINFALL = 0
    SNOW_DEPTH = 1
    EVAP = 2
    INFIL = 3
    RUNOFF = 4
    GW_FLOW = 5
    GW_ELEV = 6
    SOIL_MOIST = 7
    POLLUT_BASE = 8


class OutNodeVar(IntEnum):
    """Node output result variable indices."""
    DEPTH = 0
    HEAD = 1
    VOLUME = 2
    LATERAL_INFLOW = 3
    TOTAL_INFLOW = 4
    OVERFLOW = 5
    POLLUT_BASE = 6


class OutLinkVar(IntEnum):
    """Link output result variable indices."""
    FLOW = 0
    DEPTH = 1
    VELOCITY = 2
    VOLUME = 3
    CAPACITY = 4
    POLLUT_BASE = 5


class OutSystemVar(IntEnum):
    """System-wide output result variable indices."""
    TEMPERATURE = 0
    RAINFALL = 1
    SNOW_DEPTH = 2
    EVAP = 3
    INFIL = 4
    RUNOFF = 5
    DW_INFLOW = 6
    GW_INFLOW = 7
    LAT_INFLOW = 8
    FLOODING = 9
    OUTFLOW = 10
    STORAGE = 11
    EVAP_TOTAL = 12
    PET = 13


class WarnCode(IntEnum):
    """Engine warning codes emitted via the warning callback.

    :cvar NONE: No warning.
    :cvar HOTSTART_MISSING: Object missing during hot start application.
    :cvar UNKNOWN_SECTION: Unrecognised input section encountered.
    :cvar UNKNOWN_OPTION: Unrecognised option keyword.
    :cvar DEPRECATED_KW: Deprecated keyword used.
    :cvar PLUGIN_INIT: Plugin initialisation issue.
    :cvar NUMERICAL: Numerical instability handled gracefully.
    :cvar STABILITY_LIMIT: Timestep limited by stability criterion.
    """
    NONE = 0
    HOTSTART_MISSING = 1
    UNKNOWN_SECTION = 2
    UNKNOWN_OPTION = 3
    DEPRECATED_KW = 4
    PLUGIN_INIT = 5
    NUMERICAL = 6
    STABILITY_LIMIT = 7


class ObjectType(IntEnum):
    """SWMM object type codes.

    :cvar GAGE: Rain gage.
    :cvar SUBCATCH: Subcatchment.
    :cvar NODE: Node (junction, outfall, storage, divider).
    :cvar LINK: Link (conduit, pump, orifice, weir, outlet).
    :cvar POLLUT: Pollutant.
    :cvar LANDUSE: Land use category.
    :cvar TIMESER: Time series.
    :cvar TABLE: Curve / table.
    :cvar RDII: RDII unit hydrograph group.
    :cvar UNITHYD: Unit hydrograph.
    :cvar SNOWMELT: Snowmelt parameter set.
    :cvar SHAPE: Custom cross-section shape.
    :cvar LID: LID control.
    """
    GAGE = 0
    SUBCATCH = 1
    NODE = 2
    LINK = 3
    POLLUT = 4
    LANDUSE = 5
    TIMESER = 6
    TABLE = 7
    RDII = 8
    UNITHYD = 9
    SNOWMELT = 10
    SHAPE = 11
    LID = 12
