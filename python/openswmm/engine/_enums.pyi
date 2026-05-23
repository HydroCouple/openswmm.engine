"""
Enumerations
============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for L{openswmm.engine._enums}.

Integer-backed enums mirroring the C API enum definitions in
``openswmm_engine.h``. All enums inherit from L{enum.IntEnum} and
can be compared directly with integer return values from C API functions.
"""

from enum import IntEnum


# =============================================================================
# Lifecycle / errors / object types
# =============================================================================

class ErrorCode(IntEnum):
    """SWMM C API return codes.

    Mirrors the integer error codes returned by every C API entry point in
    ``openswmm_engine.h``.

    @cvar OK: Success (0).
    @cvar NOMEM: Out of memory.
    @cvar INPFILE: Cannot open input file.
    @cvar RPTFILE: Cannot open report file.
    @cvar OUTFILE: Cannot open output file.
    @cvar PARSE: Input file parse error.
    @cvar LIFECYCLE: Function called in wrong lifecycle state.
    @cvar BADHANDLE: NULL or invalid engine handle.
    @cvar BADINDEX: Object index out of range.
    @cvar BADPARAM: Invalid parameter value.
    @cvar PLUGIN: Plugin error.
    @cvar IO: I/O error.
    @cvar HOTSTART: Hot start file error.
    @cvar CRS: Coordinate reference system error.
    @cvar NUMERICAL: Numerical error.
    @cvar INTERNAL: Internal error.
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

    Values match the C++ internal C{openswmm::EngineState} enum (which is
    what C{swmm_engine_get_state} actually returns).

    @cvar CREATED: Context allocated, no input loaded.
    @cvar OPENED: Input file parsed, objects allocated.
    @cvar INITIALIZED: Initial conditions applied.
    @cvar RUNNING: Simulation loop in progress.
    @cvar PAUSED: Simulation paused (future hot-swap support).
    @cvar ENDED: Simulation loop completed.
    @cvar REPORTED: Summary report written.
    @cvar CLOSED: Resources released.
    @cvar ERROR_STATE: Fatal error.
    @cvar BUILDING: Programmatic model construction in progress (no .inp).
    """

    CREATED = 0
    OPENED = 1
    INITIALIZED = 2
    RUNNING = 3
    PAUSED = 4
    ENDED = 5
    REPORTED = 6
    CLOSED = 7
    ERROR_STATE = 8
    BUILDING = 9


class WarnCode(IntEnum):
    """Engine warning codes emitted via the warning callback.

    @cvar NONE: No warning.
    @cvar HOTSTART_MISSING: Object missing during hot start application.
    @cvar UNKNOWN_SECTION: Unrecognised input section encountered.
    @cvar UNKNOWN_OPTION: Unrecognised option keyword.
    @cvar DEPRECATED_KW: Deprecated keyword used.
    @cvar PLUGIN_INIT: Plugin initialisation issue.
    @cvar NUMERICAL: Numerical instability handled gracefully.
    @cvar STABILITY_LIMIT: Timestep limited by stability criterion.
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

    @cvar GAGE: Rain gage.
    @cvar SUBCATCH: Subcatchment.
    @cvar NODE: Node (junction, outfall, storage, divider).
    @cvar LINK: Link (conduit, pump, orifice, weir, outlet).
    @cvar POLLUT: Pollutant.
    @cvar LANDUSE: Land use category.
    @cvar TIMESER: Time series.
    @cvar TABLE: Curve / table.
    @cvar RDII: RDII unit hydrograph group.
    @cvar UNITHYD: Unit hydrograph.
    @cvar SNOWMELT: Snowmelt parameter set.
    @cvar SHAPE: Custom cross-section shape.
    @cvar LID: LID control.
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


# =============================================================================
# Hydraulics
# =============================================================================

class FlowUnits(IntEnum):
    """Flow unit systems.

    @cvar CFS: Cubic feet per second (US customary).
    @cvar GPM: Gallons per minute (US customary).
    @cvar MGD: Million gallons per day (US customary).
    @cvar CMS: Cubic meters per second (SI).
    @cvar LPS: Liters per second (SI).
    @cvar MLD: Million liters per day (SI).
    """

    CFS = 0
    GPM = 1
    MGD = 2
    CMS = 3
    LPS = 4
    MLD = 5


class RouteModel(IntEnum):
    """Hydraulic routing models.

    @cvar STEADY: Steady-state routing.
    @cvar KINWAVE: Kinematic wave routing.
    @cvar DYNWAVE: Dynamic wave (full Saint-Venant) routing.
    """

    STEADY = 0
    KINWAVE = 1
    DYNWAVE = 2


class NodeType(IntEnum):
    """Node type codes for L{openswmm.engine.ModelBuilder.add_node}.

    @cvar JUNCTION: Junction node.
    @cvar OUTFALL: Outfall node.
    @cvar STORAGE: Storage unit.
    @cvar DIVIDER: Flow divider.
    """

    JUNCTION = 0
    OUTFALL = 1
    STORAGE = 2
    DIVIDER = 3


class LinkType(IntEnum):
    """Link type codes for L{openswmm.engine.ModelBuilder.add_link}.

    @cvar CONDUIT: Conduit (pipe or channel).
    @cvar PUMP: Pump.
    @cvar ORIFICE: Orifice.
    @cvar WEIR: Weir.
    @cvar OUTLET: Outlet.
    """

    CONDUIT = 0
    PUMP = 1
    ORIFICE = 2
    WEIR = 3
    OUTLET = 4


class OutfallType(IntEnum):
    """Outfall boundary condition type.

    @cvar FREE: Free outfall.
    @cvar NORMAL: Normal depth outfall.
    @cvar FIXED: Fixed head outfall.
    @cvar TIDAL: Tidal stage outfall.
    @cvar TIMESERIES: Time-series stage outfall.
    """

    FREE = 0
    NORMAL = 1
    FIXED = 2
    TIDAL = 3
    TIMESERIES = 4


class XSectShape(IntEnum):
    """Cross-section shape codes.

    @cvar CIRCULAR: Circular pipe.
    @cvar FILLED_CIRCULAR: Filled circular pipe.
    @cvar RECT_CLOSED: Closed rectangular.
    @cvar RECT_OPEN: Open rectangular.
    @cvar TRAPEZOIDAL: Trapezoidal channel.
    @cvar TRIANGULAR: Triangular channel.
    @cvar PARABOLIC: Parabolic channel.
    @cvar POWER: Power-law shaped channel.
    @cvar MODBASKETHANDLE: Modified baskethandle.
    @cvar EGGSHAPED: Egg-shaped pipe.
    @cvar HORSESHOE: Horseshoe-shaped pipe.
    @cvar GOTHIC: Gothic arch pipe.
    @cvar CATENARY: Catenary-shaped pipe.
    @cvar SEMIELLIPTICAL: Semi-elliptical pipe.
    @cvar BASKETHANDLE: Baskethandle-shaped pipe.
    @cvar SEMICIRCULAR: Semi-circular pipe.
    @cvar IRREGULAR: Irregular (from transect data).
    @cvar CUSTOM: Custom shape (from shape curve).
    @cvar FORCE_MAIN: Force main (pressurized).
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


# =============================================================================
# Hydrology
# =============================================================================

class InfilModel(IntEnum):
    """Infiltration model type.

    @cvar HORTON: Original Horton model.
    @cvar MOD_HORTON: Modified Horton model.
    @cvar GREEN_AMPT: Green-Ampt model.
    @cvar MOD_GREEN_AMPT: Modified Green-Ampt model.
    @cvar CURVE_NUMBER: SCS Curve Number model.
    """

    HORTON = 0
    MOD_HORTON = 1
    GREEN_AMPT = 2
    MOD_GREEN_AMPT = 3
    CURVE_NUMBER = 4


class GageDataSource(IntEnum):
    """Rain gage data source type.

    @cvar TIMESERIES: Data comes from a time series object.
    @cvar FILE: Data comes from an external rainfall file.
    """

    TIMESERIES = 0
    FILE = 1


class GageRainType(IntEnum):
    """Rain gage rainfall data format.

    @cvar INTENSITY: Rainfall intensity (depth/time).
    @cvar VOLUME: Rainfall volume (depth per interval).
    @cvar CUMULATIVE: Cumulative rainfall depth.
    """

    INTENSITY = 0
    VOLUME = 1
    CUMULATIVE = 2


# =============================================================================
# Water quality and LID
# =============================================================================

class ConcentrationUnits(IntEnum):
    """Pollutant concentration units.

    @cvar MG_PER_L: Milligrams per liter.
    @cvar UG_PER_L: Micrograms per liter.
    @cvar COUNT_PER_L: Counts per liter.
    """

    MG_PER_L = 0
    UG_PER_L = 1
    COUNT_PER_L = 2


class BuildupFunc(IntEnum):
    """Pollutant buildup function type.

    @cvar NONE: No buildup.
    @cvar POW: Power function.
    @cvar EXP: Exponential function.
    @cvar SAT: Saturation function.
    @cvar EXT: External time series.
    """

    NONE = 0
    POW = 1
    EXP = 2
    SAT = 3
    EXT = 4


class WashoffFunc(IntEnum):
    """Pollutant washoff function type.

    @cvar NONE: No washoff.
    @cvar EXP: Exponential washoff.
    @cvar RC: Rating curve washoff.
    @cvar EMC: Event mean concentration.
    """

    NONE = 0
    EXP = 1
    RC = 2
    EMC = 3


class LidType(IntEnum):
    """Low Impact Development (LID) control type.

    @cvar BIO_CELL: Bioretention cell.
    @cvar RAIN_GARDEN: Rain garden.
    @cvar GREEN_ROOF: Green roof.
    @cvar INFIL_TRENCH: Infiltration trench.
    @cvar PERM_PAVEMENT: Permeable pavement.
    @cvar RAIN_BARREL: Rain barrel.
    @cvar ROOFTOP_DISCONN: Rooftop disconnection.
    @cvar VEGETATIVE_SWALE: Vegetative swale.
    """

    BIO_CELL = 0
    RAIN_GARDEN = 1
    GREEN_ROOF = 2
    INFIL_TRENCH = 3
    PERM_PAVEMENT = 4
    RAIN_BARREL = 5
    ROOFTOP_DISCONN = 6
    VEGETATIVE_SWALE = 7


# =============================================================================
# Output variables
# =============================================================================

class OutSubcatchVar(IntEnum):
    """Subcatchment output result variable indices.

    @cvar RAINFALL: Rainfall rate.
    @cvar SNOW_DEPTH: Snow depth.
    @cvar EVAP: Evaporation rate.
    @cvar INFIL: Infiltration rate.
    @cvar RUNOFF: Runoff rate.
    @cvar GW_FLOW: Groundwater outflow rate.
    @cvar GW_ELEV: Groundwater table elevation.
    @cvar SOIL_MOIST: Soil moisture fraction.
    @cvar POLLUT_BASE: Base index for pollutant concentrations.
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

    @cvar DEPTH: Water depth.
    @cvar HEAD: Hydraulic head.
    @cvar VOLUME: Stored volume.
    @cvar LATERAL_INFLOW: Lateral inflow rate.
    @cvar TOTAL_INFLOW: Total inflow rate.
    @cvar OVERFLOW: Overflow / flooding rate.
    @cvar POLLUT_BASE: Base index for pollutant concentrations.
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

    @cvar FLOW: Flow rate.
    @cvar DEPTH: Water depth.
    @cvar VELOCITY: Flow velocity.
    @cvar VOLUME: Stored volume.
    @cvar CAPACITY: Capacity fraction (flow / full flow).
    @cvar POLLUT_BASE: Base index for pollutant concentrations.
    """

    FLOW = 0
    DEPTH = 1
    VELOCITY = 2
    VOLUME = 3
    CAPACITY = 4
    POLLUT_BASE = 5


class OutSystemVar(IntEnum):
    """System-wide output result variable indices.

    @cvar TEMPERATURE: Air temperature.
    @cvar RAINFALL: System-wide rainfall rate.
    @cvar SNOW_DEPTH: Average snow depth.
    @cvar EVAP: System-wide evaporation rate.
    @cvar INFIL: System-wide infiltration rate.
    @cvar RUNOFF: System-wide runoff rate.
    @cvar DW_INFLOW: Dry-weather inflow rate.
    @cvar GW_INFLOW: Groundwater inflow rate.
    @cvar LAT_INFLOW: Total lateral inflow rate.
    @cvar FLOODING: Total flooding rate.
    @cvar OUTFLOW: Total outfall outflow rate.
    @cvar STORAGE: Total network storage volume.
    @cvar EVAP_TOTAL: Actual evaporation rate.
    @cvar PET: Potential evapotranspiration rate.
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


# =============================================================================
# Forcing
# =============================================================================

class ForcingMode(IntEnum):
    """Forcing application mode.

    Determines how a forced value is combined with the model-computed value.

    @cvar REPLACE: Replace the computed value entirely.
    @cvar ADD: Add the forced value to the computed value.
    """

    REPLACE = 0
    ADD = 1


class ForcingTarget(IntEnum):
    """Object type codes used with L{openswmm.engine.Forcing.clear}.

    @cvar NODE: Node forcing target.
    @cvar LINK: Link forcing target.
    @cvar SUBCATCH: Subcatchment forcing target.
    @cvar GAGE: Rain gage forcing target.
    """

    NODE = 0
    LINK = 1
    SUBCATCH = 2
    GAGE = 3


# =============================================================================
# Patterns
# =============================================================================

class PatternType(IntEnum):
    """Time pattern type.

    @cvar MONTHLY: Monthly variation pattern.
    @cvar DAILY: Daily variation pattern.
    @cvar HOURLY: Hourly variation pattern.
    @cvar WEEKEND: Weekend hourly variation pattern.
    """

    MONTHLY = 0
    DAILY = 1
    HOURLY = 2
    WEEKEND = 3


# =============================================================================
# Mass-balance totals
# =============================================================================

class RunoffTotal(IntEnum):
    """Runoff mass balance component codes.

    @cvar RAINFALL: Total rainfall.
    @cvar EVAP: Evaporation loss.
    @cvar INFIL: Infiltration loss.
    @cvar RUNOFF: Surface runoff.
    @cvar SNOWREMOV: Snow removal.
    @cvar INITSTORE: Initial surface storage.
    @cvar FINALSTORE: Final surface storage.
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

    @cvar DRY_WEATHER: Dry-weather inflow.
    @cvar WET_WEATHER: Wet-weather (runoff) inflow.
    @cvar GW_INFLOW: Groundwater inflow.
    @cvar RDII: RDII inflow.
    @cvar EXTERNAL: External inflow.
    @cvar FLOODING: Flooding loss.
    @cvar OUTFLOW: Outfall outflow.
    @cvar EVAP_LOSS: Evaporation loss.
    @cvar SEEP_LOSS: Seepage loss.
    @cvar INIT_STORAGE: Initial network storage.
    @cvar FINAL_STORAGE: Final network storage.
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
