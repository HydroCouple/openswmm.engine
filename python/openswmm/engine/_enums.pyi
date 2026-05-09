"""
Enumerations
============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._enums`.

Integer-backed enums mirroring the C API enum definitions in
``openswmm_engine.h``. All enums inherit from :class:`~enum.IntEnum` and
can be compared directly with integer return values from C API functions.
"""

from enum import IntEnum


class ErrorCode(IntEnum):
    """SWMM C API return codes.

    Attributes:
        OK: Success (0).
        NOMEM: Out of memory.
        INPFILE: Cannot open input file.
        RPTFILE: Cannot open report file.
        OUTFILE: Cannot open output file.
        PARSE: Input file parse error.
        LIFECYCLE: Function called in wrong lifecycle state.
        BADHANDLE: NULL or invalid engine handle.
        BADINDEX: Object index out of range.
        BADPARAM: Invalid parameter value.
        PLUGIN: Plugin error.
        IO: I/O error.
        HOTSTART: Hot start file error.
        CRS: Coordinate reference system error.
        NUMERICAL: Numerical error.
        INTERNAL: Internal error.
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
    """Engine lifecycle states.

    Attributes:
        NONE: Not yet created.
        CREATED: Engine allocated but not opened.
        OPENED: Input file parsed.
        INITIALIZED: Arrays allocated, initial conditions set.
        STARTED: Simulation started.
        RUNNING: Simulation in progress (after first step).
        ENDED: Simulation ended.
        CLOSED: Files closed.
        BUILDING: Programmatic model construction in progress.
    """

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
    """Node type codes for :meth:`~openswmm.engine.ModelBuilder.add_node`.

    Attributes:
        JUNCTION: Junction node.
        OUTFALL: Outfall node.
        STORAGE: Storage unit.
        DIVIDER: Flow divider.
    """

    JUNCTION = 0
    OUTFALL = 1
    STORAGE = 2
    DIVIDER = 3


class LinkType(IntEnum):
    """Link type codes for :meth:`~openswmm.engine.ModelBuilder.add_link`.

    Attributes:
        CONDUIT: Conduit (pipe or channel).
        PUMP: Pump.
        ORIFICE: Orifice.
        WEIR: Weir.
        OUTLET: Outlet.
    """

    CONDUIT = 0
    PUMP = 1
    ORIFICE = 2
    WEIR = 3
    OUTLET = 4


class XSectShape(IntEnum):
    """Cross-section shape codes.

    Attributes:
        CIRCULAR: Circular pipe.
        FILLED_CIRCULAR: Filled circular pipe.
        RECT_CLOSED: Closed rectangular.
        RECT_OPEN: Open rectangular.
        TRAPEZOIDAL: Trapezoidal channel.
        TRIANGULAR: Triangular channel.
        PARABOLIC: Parabolic channel.
        POWER: Power-law shaped channel.
        MODBASKETHANDLE: Modified baskethandle.
        EGGSHAPED: Egg-shaped pipe.
        HORSESHOE: Horseshoe-shaped pipe.
        GOTHIC: Gothic arch pipe.
        CATENARY: Catenary-shaped pipe.
        SEMIELLIPTICAL: Semi-elliptical pipe.
        BASKETHANDLE: Baskethandle-shaped pipe.
        SEMICIRCULAR: Semi-circular pipe.
        IRREGULAR: Irregular (from transect data).
        CUSTOM: Custom shape (from shape curve).
        FORCE_MAIN: Force main (pressurized).
    """

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
    """Flow unit systems.

    Attributes:
        CFS: Cubic feet per second (US customary).
        GPM: Gallons per minute (US customary).
        MGD: Million gallons per day (US customary).
        CMS: Cubic meters per second (SI).
        LPS: Liters per second (SI).
        MLD: Million liters per day (SI).
    """

    CFS = 0
    GPM = 1
    MGD = 2
    CMS = 3
    LPS = 4
    MLD = 5


class RouteModel(IntEnum):
    """Hydraulic routing models.

    Attributes:
        STEADY: Steady-state routing.
        KINWAVE: Kinematic wave routing.
        DYNWAVE: Dynamic wave (full Saint-Venant) routing.
    """

    STEADY = 0
    KINWAVE = 1
    DYNWAVE = 2


class GageDataSource(IntEnum):
    """Rain gage data source type.

    Attributes:
        TIMESERIES: Data comes from a time series object.
        FILE: Data comes from an external rainfall file.
    """

    TIMESERIES = 0
    FILE = 1


class GageRainType(IntEnum):
    """Rain gage rainfall data format.

    Attributes:
        INTENSITY: Rainfall intensity (depth/time).
        VOLUME: Rainfall volume (depth per interval).
        CUMULATIVE: Cumulative rainfall depth.
    """

    INTENSITY = 0
    VOLUME = 1
    CUMULATIVE = 2


class InfilModel(IntEnum):
    """Infiltration model type.

    Attributes:
        HORTON: Original Horton model.
        MOD_HORTON: Modified Horton model.
        GREEN_AMPT: Green-Ampt model.
        MOD_GREEN_AMPT: Modified Green-Ampt model.
        CURVE_NUMBER: SCS Curve Number model.
    """

    HORTON = 0
    MOD_HORTON = 1
    GREEN_AMPT = 2
    MOD_GREEN_AMPT = 3
    CURVE_NUMBER = 4


class OutfallType(IntEnum):
    """Outfall boundary condition type.

    Attributes:
        FREE: Free outfall.
        NORMAL: Normal depth outfall.
        FIXED: Fixed head outfall.
        TIDAL: Tidal stage outfall.
        TIMESERIES: Time-series stage outfall.
    """

    FREE = 0
    NORMAL = 1
    FIXED = 2
    TIDAL = 3
    TIMESERIES = 4


class ConcentrationUnits(IntEnum):
    """Pollutant concentration units.

    Attributes:
        MG_PER_L: Milligrams per liter.
        UG_PER_L: Micrograms per liter.
        COUNT_PER_L: Counts per liter.
    """

    MG_PER_L = 0
    UG_PER_L = 1
    COUNT_PER_L = 2


class BuildupFunc(IntEnum):
    """Pollutant buildup function type.

    Attributes:
        NONE: No buildup.
        POW: Power function.
        EXP: Exponential function.
        SAT: Saturation function.
        EXT: External time series.
    """

    NONE = 0
    POW = 1
    EXP = 2
    SAT = 3
    EXT = 4


class WashoffFunc(IntEnum):
    """Pollutant washoff function type.

    Attributes:
        NONE: No washoff.
        EXP: Exponential washoff.
        RC: Rating curve washoff.
        EMC: Event mean concentration.
    """

    NONE = 0
    EXP = 1
    RC = 2
    EMC = 3


class RunoffTotal(IntEnum):
    """Runoff mass balance component codes.

    Attributes:
        RAINFALL: Total rainfall.
        EVAP: Evaporation loss.
        INFIL: Infiltration loss.
        RUNOFF: Surface runoff.
        SNOWREMOV: Snow removal.
        INITSTORE: Initial surface storage.
        FINALSTORE: Final surface storage.
    """

    RAINFALL = 0
    EVAP = 1
    INFIL = 2
    RUNOFF = 3
    SNOWREMOV = 4
    INITSTORE = 5
    FINALSTORE = 6


class RoutingTotal(IntEnum):
    """Routing mass balance component codes.

    Attributes:
        DRY_WEATHER: Dry-weather inflow.
        WET_WEATHER: Wet-weather (runoff) inflow.
        GW_INFLOW: Groundwater inflow.
        RDII: RDII inflow.
        EXTERNAL: External inflow.
        FLOODING: Flooding loss.
        OUTFLOW: Outfall outflow.
        EVAP_LOSS: Evaporation loss.
        SEEP_LOSS: Seepage loss.
        INIT_STORAGE: Initial network storage.
        FINAL_STORAGE: Final network storage.
    """

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
    """Low Impact Development (LID) control type.

    Attributes:
        BIO_CELL: Bioretention cell.
        RAIN_GARDEN: Rain garden.
        GREEN_ROOF: Green roof.
        INFIL_TRENCH: Infiltration trench.
        PERM_PAVEMENT: Permeable pavement.
        RAIN_BARREL: Rain barrel.
        ROOFTOP_DISCONN: Rooftop disconnection.
        VEGETATIVE_SWALE: Vegetative swale.
    """

    BIO_CELL = 0
    RAIN_GARDEN = 1
    GREEN_ROOF = 2
    INFIL_TRENCH = 3
    PERM_PAVEMENT = 4
    RAIN_BARREL = 5
    ROOFTOP_DISCONN = 6
    VEGETATIVE_SWALE = 7


class PatternType(IntEnum):
    """Time pattern type.

    Attributes:
        MONTHLY: Monthly variation pattern.
        DAILY: Daily variation pattern.
        HOURLY: Hourly variation pattern.
        WEEKEND: Weekend hourly variation pattern.
    """

    MONTHLY = 0
    DAILY = 1
    HOURLY = 2
    WEEKEND = 3


class ForcingMode(IntEnum):
    """Forcing application mode.

    Determines how a forced value is combined with the model-computed value.

    Attributes:
        REPLACE: Replace the computed value entirely.
        ADD: Add the forced value to the computed value.
    """

    REPLACE = 0
    ADD = 1


class ForcingTarget(IntEnum):
    """Object type codes used with :meth:`~openswmm.engine.Forcing.clear`.

    Attributes:
        NODE: Node forcing target.
        LINK: Link forcing target.
        SUBCATCH: Subcatchment forcing target.
        GAGE: Rain gage forcing target.
    """

    NODE = 0
    LINK = 1
    SUBCATCH = 2
    GAGE = 3


class OutSubcatchVar(IntEnum):
    """Subcatchment output result variable indices.

    Attributes:
        RAINFALL: Rainfall rate.
        SNOW_DEPTH: Snow depth.
        EVAP: Evaporation rate.
        INFIL: Infiltration rate.
        RUNOFF: Runoff rate.
        GW_FLOW: Groundwater outflow rate.
        GW_ELEV: Groundwater table elevation.
        SOIL_MOIST: Soil moisture fraction.
        POLLUT_BASE: Base index for pollutant concentrations.
    """

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
    """Node output result variable indices.

    Attributes:
        DEPTH: Water depth.
        HEAD: Hydraulic head.
        VOLUME: Stored volume.
        LATERAL_INFLOW: Lateral inflow rate.
        TOTAL_INFLOW: Total inflow rate.
        OVERFLOW: Overflow / flooding rate.
        POLLUT_BASE: Base index for pollutant concentrations.
    """

    DEPTH = 0
    HEAD = 1
    VOLUME = 2
    LATERAL_INFLOW = 3
    TOTAL_INFLOW = 4
    OVERFLOW = 5
    POLLUT_BASE = 6


class OutLinkVar(IntEnum):
    """Link output result variable indices.

    Attributes:
        FLOW: Flow rate.
        DEPTH: Water depth.
        VELOCITY: Flow velocity.
        VOLUME: Stored volume.
        CAPACITY: Capacity fraction (flow / full flow).
        POLLUT_BASE: Base index for pollutant concentrations.
    """

    FLOW = 0
    DEPTH = 1
    VELOCITY = 2
    VOLUME = 3
    CAPACITY = 4
    POLLUT_BASE = 5


class OutSystemVar(IntEnum):
    """System-wide output result variable indices.

    Attributes:
        TEMPERATURE: Air temperature.
        RAINFALL: System-wide rainfall rate.
        SNOW_DEPTH: Average snow depth.
        EVAP: System-wide evaporation rate.
        INFIL: System-wide infiltration rate.
        RUNOFF: System-wide runoff rate.
        DW_INFLOW: Dry-weather inflow rate.
        GW_INFLOW: Groundwater inflow rate.
        LAT_INFLOW: Total lateral inflow rate.
        FLOODING: Total flooding rate.
        OUTFLOW: Total outfall outflow rate.
        STORAGE: Total network storage volume.
        EVAP_TOTAL: Actual evaporation rate.
        PET: Potential evapotranspiration rate.
    """

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

    Attributes:
        NONE: No warning.
        HOTSTART_MISSING: Object missing during hot start application.
        UNKNOWN_SECTION: Unrecognised input section encountered.
        UNKNOWN_OPTION: Unrecognised option keyword.
        DEPRECATED_KW: Deprecated keyword used.
        PLUGIN_INIT: Plugin initialisation issue.
        NUMERICAL: Numerical instability handled gracefully.
        STABILITY_LIMIT: Timestep limited by stability criterion.
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

    Attributes:
        GAGE: Rain gage.
        SUBCATCH: Subcatchment.
        NODE: Node (junction, outfall, storage, divider).
        LINK: Link (conduit, pump, orifice, weir, outlet).
        POLLUT: Pollutant.
        LANDUSE: Land use category.
        TIMESER: Time series.
        TABLE: Curve / table.
        RDII: RDII unit hydrograph group.
        UNITHYD: Unit hydrograph.
        SNOWMELT: Snowmelt parameter set.
        SHAPE: Custom cross-section shape.
        LID: LID control.
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
