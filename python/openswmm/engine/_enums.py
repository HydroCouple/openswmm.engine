"""
Enumerations
============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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


# =============================================================================
# Lifecycle / errors / object types
# =============================================================================

class ErrorCode(IntEnum):
    """SWMM C API return codes.

    Mirrors the integer error codes returned by every C API entry point in
    ``openswmm_engine.h``. A non-zero value generally indicates a failure
    that the binding layer translates into an L{EngineError}.

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
    @cvar NUMERICAL: Numerical error (e.g., divergence).
    @cvar DEPENDENCY: Object has dependents that block the requested operation.
    @cvar INTERNAL: Internal/unspecified error.
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
    DEPENDENCY = 15
    INTERNAL = 99


class EngineState(IntEnum):
    """Engine lifecycle states.

    Returned by the C{state} property of L{Solver}. Values mirror the
    C{SWMM_EngineState} enum in C{openswmm_engine.h}.

    @cvar NONE: Uninitialised / fatal-error sentinel.
    @cvar CREATED: Context allocated, no input loaded.
    @cvar OPENED: Input file parsed, objects allocated.
    @cvar INITIALIZED: Initial conditions applied.
    @cvar STARTED: Simulation prepared to step (post-start, pre-first-step).
    @cvar RUNNING: Simulation loop in progress.
    @cvar ENDED: Simulation loop completed.
    @cvar CLOSED: Resources released.
    @cvar BUILDING: Programmatic model construction in progress (no .inp).
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

    Used wherever the C API accepts a generic object-type discriminator.

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

    Determines the unit system used throughout the simulation. The first
    three are US customary; the last three are SI.

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
    """SWMM node type codes.

    Used wherever the C API returns or accepts an integer node-type field.

    @cvar JUNCTION: Manhole / generic junction (most common).
    @cvar OUTFALL: Drainage system terminus (boundary condition).
    @cvar STORAGE: Storage unit with stage-area relationship.
    @cvar DIVIDER: Flow divider (steady-flow routing only).
    """

    JUNCTION = 0
    OUTFALL = 1
    STORAGE = 2
    DIVIDER = 3


class LinkType(IntEnum):
    """SWMM link type codes.

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


class OrificeType(IntEnum):
    """Orifice flow-attack classification.

    Mirrors ``SWMM_OrificeType`` in ``openswmm_links.h``.

    @cvar SIDE: Orifice opens on the side of the upstream node.
    @cvar BOTTOM: Orifice opens through the bottom of the upstream node.
    """

    SIDE = 0
    BOTTOM = 1


class WeirType(IntEnum):
    """Weir flow classification.

    Mirrors ``SWMM_WeirType`` in ``openswmm_links.h``.

    @cvar TRANSVERSE: Sharp-crested transverse weir.
    @cvar SIDEFLOW: Side-flow weir (USBR formula).
    @cvar VNOTCH: Triangular / V-notch weir.
    @cvar TRAPEZOIDAL: Trapezoidal weir.
    @cvar ROADWAY: FHWA HDS-5 roadway weir.
    """

    TRANSVERSE = 0
    SIDEFLOW = 1
    VNOTCH = 2
    TRAPEZOIDAL = 3
    ROADWAY = 4


class OutletRatingType(IntEnum):
    """Outlet rating-curve classification.

    Mirrors ``SWMM_OutletRatingType`` in ``openswmm_links.h``.

    @cvar FUNCTIONAL_HEAD: ``Q = Cd · H^expon`` (head above invert).
    @cvar FUNCTIONAL_DEPTH: ``Q = Cd · y^expon`` (depth at upstream node).
    @cvar TABULAR_HEAD: ``Q`` from rating curve indexed by head.
    @cvar TABULAR_DEPTH: ``Q`` from rating curve indexed by depth.
    """

    FUNCTIONAL_HEAD = 0
    FUNCTIONAL_DEPTH = 1
    TABULAR_HEAD = 2
    TABULAR_DEPTH = 3


class OutfallType(IntEnum):
    """Outfall boundary condition type.

    @cvar FREE: Free outfall (critical or normal depth, whichever is lower).
    @cvar NORMAL: Normal depth outfall.
    @cvar FIXED: Fixed head outfall.
    @cvar TIDAL: Tidal stage outfall (sinusoidal forcing).
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
    """LID (Low Impact Development) control type.

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


class AquiferParam(IntEnum):
    """Aquifer parameter codes for C{Aquifers.get_param} / C{set_param}.

    Values use input-file units (the C{[AQUIFERS]} line columns). The
    flux-coefficient parameters (C{CONDUCTIVITY}, C{CONDUCT_SLOPE},
    C{TENSION_SLOPE}, C{UPPER_EVAP_FRAC}, C{LOWER_EVAP_DEPTH},
    C{LOWER_LOSS_COEFF}) are settable both before the simulation starts and
    while it is running. The structural / initial-condition parameters
    (C{POROSITY}, C{WILTING_POINT}, C{FIELD_CAPACITY}, C{BOTTOM_ELEV},
    C{WATER_TABLE_ELEV}, C{UPPER_MOISTURE}) bound or seed the groundwater
    state and are pre-start-only.

    @cvar POROSITY: Porosity (volumetric fraction).
    @cvar WILTING_POINT: Wilting point (volumetric fraction).
    @cvar FIELD_CAPACITY: Field capacity (volumetric fraction).
    @cvar CONDUCTIVITY: Saturated hydraulic conductivity.
    @cvar CONDUCT_SLOPE: Conductivity slope.
    @cvar TENSION_SLOPE: Tension slope.
    @cvar UPPER_EVAP_FRAC: Upper-zone evaporation fraction.
    @cvar LOWER_EVAP_DEPTH: Lower-zone evaporation depth.
    @cvar LOWER_LOSS_COEFF: Lower-zone seepage-loss coefficient.
    @cvar BOTTOM_ELEV: Aquifer bottom elevation.
    @cvar WATER_TABLE_ELEV: Initial water table elevation.
    @cvar UPPER_MOISTURE: Initial upper-zone moisture.
    """

    POROSITY = 0
    WILTING_POINT = 1
    FIELD_CAPACITY = 2
    CONDUCTIVITY = 3
    CONDUCT_SLOPE = 4
    TENSION_SLOPE = 5
    UPPER_EVAP_FRAC = 6
    LOWER_EVAP_DEPTH = 7
    LOWER_LOSS_COEFF = 8
    BOTTOM_ELEV = 9
    WATER_TABLE_ELEV = 10
    UPPER_MOISTURE = 11


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
    Mirrors C{SWMM_ForcingMode} in C{openswmm_forcing.h} (OVERRIDE=1, ADD=2;
    0 is the engine-internal "no forcing" state and is not exposed).

    @cvar REPLACE: Replace the computed value entirely.
    @cvar ADD: Add the forced value to the computed value.
    """

    REPLACE = 1
    ADD = 2


class ForcingTarget(IntEnum):
    """Object type codes used with L{Forcing.clear}.

    @cvar NODE: Node forcing.
    @cvar LINK: Link forcing.
    @cvar SUBCATCH: Subcatchment forcing.
    @cvar GAGE: Rain gage forcing.
    @cvar CLIMATE: System-wide climate forcing (temperature, wind).
    """

    NODE = 0
    LINK = 1
    SUBCATCH = 2
    GAGE = 3
    CLIMATE = 4


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
    @cvar FORCING_INFLOW: Runtime-API forced lateral inflow (e.g.
        flow injected via Nodes.set_lateral_inflow / transient
        ForcingData). Distinct from EXTERNAL which only counts INP
        [INFLOWS]-derived inflow.
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
    FORCING_INFLOW = 11


# =============================================================================
# Dividers
# =============================================================================


class DividerType(IntEnum):
    """Flow-diversion method for a C{DIVIDER} node.

    Mirrors C{SWMM_DividerType} in C{openswmm_nodes.h}.

    @cvar CUTOFF: Flow above a cutoff value is diverted.
    @cvar OVERFLOW: Diverted flow equals the capacity exceedance of the
        main link.
    @cvar TABULAR: Diverted flow is looked up on a diversion curve.
    @cvar WEIR: A weir equation governs the diversion.
    """

    CUTOFF = 0
    OVERFLOW = 1
    TABULAR = 2
    WEIR = 3


# =============================================================================
# Runtime forcing
# =============================================================================


class ForcingType(IntEnum):
    """Forcing channel selected when injecting a runtime override.

    Mirrors C{SWMM_ForcingType} in C{openswmm_forcing.h}. Distinct from
    L{ForcingTarget}, which only names the object *kind* passed to
    L{Forcing.clear}.

    @cvar NODE_LAT_INFLOW: Lateral inflow at a node.
    @cvar NODE_HEAD_BOUNDARY: Head boundary condition at a node.
    @cvar NODE_QUALITY: Pollutant concentration at a node.
    @cvar LINK_FLOW: Imposed flow on a link.
    @cvar LINK_SETTING: Control setting on a link.
    @cvar SUBCATCH_RAINFALL: Rainfall on a subcatchment.
    @cvar SUBCATCH_EVAP: Evaporation on a subcatchment.
    @cvar GAGE_RAINFALL: Rainfall at a rain gage.
    @cvar CLIMATE_TEMPERATURE: System-wide air temperature.
    @cvar CLIMATE_WIND: System-wide wind speed.
    @cvar SUBCATCH_SNOWFALL: Snowfall on a subcatchment.
    @cvar CLIMATE_EVAP: System-wide evaporation rate.
    @cvar LINK_QUALITY: Pollutant quality on a link.
    """

    NODE_LAT_INFLOW = 0
    NODE_HEAD_BOUNDARY = 1
    NODE_QUALITY = 2
    LINK_FLOW = 3
    LINK_SETTING = 4
    SUBCATCH_RAINFALL = 5
    SUBCATCH_EVAP = 6
    GAGE_RAINFALL = 7
    CLIMATE_TEMPERATURE = 8
    CLIMATE_WIND = 9
    SUBCATCH_SNOWFALL = 10
    CLIMATE_EVAP = 11
    LINK_QUALITY = 12


class ForcingPersist(IntEnum):
    """Lifetime of a runtime forcing override.

    Mirrors C{SWMM_ForcingPersist} in C{openswmm_forcing.h}. Shared by the 1D
    and 2D forcing APIs.

    @cvar RESET: Auto-clear the forcing after each routing step.
    @cvar PERSIST: Keep the forcing until it is explicitly cleared.
    """

    RESET = 0
    PERSIST = 1


# =============================================================================
# 2D surface routing
# =============================================================================

class SurfaceForcingMode(IntEnum):
    """How a 2D surface forcing value is applied to a mesh cell.

    Mirrors C{SWMM_ForcingMode} in C{openswmm_forcing.h} and the engine's
    C{openswmm::ForcingMode}. Note the values differ from the 1D
    L{ForcingMode}: the 2D forcing API consumes the canonical
    C{SWMM_FORCING_*} codes directly (C{OVERRIDE=1}, C{ADD=2}).

    @cvar NONE: No forcing — use the model-computed value.
    @cvar OVERRIDE: Replace the computed value with the user value.
    @cvar ADD: Add the user value to the computed value.
    """

    NONE = 0
    OVERRIDE = 1
    ADD = 2


class SurfaceBoundaryType(IntEnum):
    """2D mesh edge boundary-condition type.

    Mirrors C{openswmm::twoD::BoundaryType} (C{BoundaryData.hpp}).

    @cvar WALL: Zero-flux wall (default).
    @cvar NORMAL_FLOW: Manning outflow using the bed slope.
    @cvar SPECIFIED_STAGE: Prescribed water-surface elevation (const or TS).
    @cvar SPECIFIED_FLOW: Prescribed per-metre discharge (const or TS).
    @cvar RATING_CURVE: Stage-to-flow lookup curve.
    """

    WALL = 0
    NORMAL_FLOW = 1
    SPECIFIED_STAGE = 2
    SPECIFIED_FLOW = 3
    RATING_CURVE = 4


# =============================================================================
# Object references (model editing)
# =============================================================================


class RefType(IntEnum):
    """Kind of object that holds a reference, used by the editing/impact API.

    Mirrors C{SWMM_RefType} in C{openswmm_edit.h}.

    @cvar NODE: A node holds the reference.
    @cvar LINK: A link holds the reference.
    @cvar SUBCATCH: A subcatchment holds the reference.
    @cvar GAGE: A rain gage holds the reference.
    @cvar TABLE: A time series or curve holds the reference.
    @cvar TRANSECT: A transect holds the reference.
    @cvar INLET_USAGE: An inlet-usage entry holds the reference.
    """

    NODE = 0
    LINK = 1
    SUBCATCH = 2
    GAGE = 3
    TABLE = 4
    TRANSECT = 5
    INLET_USAGE = 6


class TableType(IntEnum):
    """Table type codes returned by C{swmm_table_get_type}.

    Tables (time series and curves) are stored in a single unified array;
    this code partitions that array. Mirrors C{openswmm::TableType}.

    @cvar TIMESERIES: Time-varying values (rainfall, inflow, etc.).
    @cvar CURVE_STORAGE: Storage node volume-depth curve.
    @cvar CURVE_DIVERSION: Diversion rating curve.
    @cvar CURVE_RATING: Outfall/weir rating curve.
    @cvar CURVE_SHAPE: Cross-section shape curve.
    @cvar CURVE_CONTROL: Control rule action curve.
    @cvar CURVE_TIDAL: Tidal stage curve.
    @cvar CURVE_PUMP1: Pump curve type 1 (ON/OFF depth).
    @cvar CURVE_PUMP2: Pump curve type 2 (head vs flow).
    @cvar CURVE_PUMP3: Pump curve type 3 (volume vs time).
    @cvar CURVE_PUMP4: Pump curve type 4 (depth vs speed).
    @cvar CURVE_PUMP5: Pump curve type 5 (head vs flow, variable speed).
    """

    TIMESERIES = 0
    CURVE_STORAGE = 1
    CURVE_DIVERSION = 2
    CURVE_RATING = 3
    CURVE_SHAPE = 4
    CURVE_CONTROL = 5
    CURVE_TIDAL = 6
    CURVE_PUMP1 = 7
    CURVE_PUMP2 = 8
    CURVE_PUMP3 = 9
    CURVE_PUMP4 = 10
    CURVE_PUMP5 = 11


class FilePathRole(IntEnum):
    """External-file slot selector for C{swmm_file_path_get/set}.

    Mirrors C{SWMM_FilePathRole}. Scalar slots ignore the ``owner``
    argument; vector slots use ``owner`` to select the entry (a decimal
    index for hot-start saves, the gage id for rain-gage data, the series
    id for time-series data).

    @cvar RAINFALL: Rainfall interface file (scalar).
    @cvar RUNOFF: Runoff interface file (scalar).
    @cvar RDII: RDII interface file (scalar).
    @cvar INFLOWS: Routing inflows interface file (scalar).
    @cvar OUTFLOWS: Routing outflows interface file (scalar).
    @cvar HOTSTART_USE: Hot-start file to read (scalar).
    @cvar CLIMATE_TEMP: Climate/temperature file (scalar).
    @cvar HOTSTART_SAVE: Hot-start save slot (vector; owner = index).
    @cvar RAINGAGE_DATA: Rain-gage data file (vector; owner = gage id).
    @cvar TIMESERIES_DATA: Time-series data file (vector; owner = series id).
    """

    RAINFALL = 1
    RUNOFF = 2
    RDII = 3
    INFLOWS = 4
    OUTFLOWS = 5
    HOTSTART_USE = 6
    CLIMATE_TEMP = 7
    HOTSTART_SAVE = 8
    RAINGAGE_DATA = 9
    TIMESERIES_DATA = 10


class UserFlagType(IntEnum):
    """User-flag schema value type for C{swmm_userflag_define}.

    Mirrors C{openswmm::UserFlagType}.

    @cvar BOOLEAN: Boolean flag (INP encoding C{YES}/C{NO}).
    @cvar INTEGER: Integer flag.
    @cvar REAL: Real-valued flag.
    @cvar STRING: Free-text flag (stored verbatim).
    """

    BOOLEAN = 0
    INTEGER = 1
    REAL = 2
    STRING = 3
