# cython: language_level=3str
# Description: Cython module for openswmmcore solver
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# python and cython imports
from enum import Enum
from warnings import warn
from typing import List, Tuple, Union, Dict, Set, Callable
from cpython.datetime cimport datetime as cython_datetime
from datetime import datetime, timedelta
from libc.stdlib cimport free, malloc
from functools import partialmethod

# external imports
from .solver cimport (
    PyObject_CallObject,
    clock_t,
    clock,
    swmm_Object,
    swmm_NodeType,
    swmm_LinkType,
    swmm_GageProperty,
    swmm_SubcatchProperty,
    swmm_NodeProperty,
    swmm_LinkProperty,
    swmm_SystemProperty,
    swmm_FlowUnitsProperty,
    swmm_API_Errors,
    progress_callback,
    swmm_run,
    swmm_run_with_callback,
    swmm_open,
    swmm_start,
    swmm_step,
    swmm_stride,
    swmm_useHotStart,
    swmm_saveHotStart,
    swmm_end,
    swmm_report,
    swmm_close,
    swmm_getMassBalErr,
    swmm_getVersion,
    swmm_getError,
    swmm_getErrorFromCode,
    swmm_getWarnings,
    swmm_getCount,
    swmm_getName,
    swmm_getIndex,
    swmm_getValue,
    swmm_getValueExpanded,
    swmm_setValue,
    swmm_setValueExpanded,
    swmm_getSavedValue,
    swmm_writeLine,
    swmm_decodeDate,
    swmm_encodeDate
)

class SWMMObjects(Enum):
    """
    Enumeration of the top-level SWMM object categories.

    @ivar RAIN_GAGE: Rain gage object.
    @type RAIN_GAGE: int
    @ivar SUBCATCHMENT: Subcatchment object.
    @type SUBCATCHMENT: int
    @ivar NODE: Node object (junction, outfall, storage, or divider).
    @type NODE: int
    @ivar LINK: Link object (conduit, pump, orifice, weir, or outlet).
    @type LINK: int
    @ivar AQUIFER: Aquifer object.
    @type AQUIFER: int
    @ivar SNOWPACK: Snowpack object.
    @type SNOWPACK: int
    @ivar UNIT_HYDROGRAPH: Unit hydrograph object.
    @type UNIT_HYDROGRAPH: int
    @ivar LID: Low Impact Development (LID) control object.
    @type LID: int
    @ivar STREET: Street object.
    @type STREET: int
    @ivar INLET: Inlet object.
    @type INLET: int
    @ivar TRANSECT: Transect (irregular cross-section) object.
    @type TRANSECT: int
    @ivar XSECTION_SHAPE: Cross-section shape object.
    @type XSECTION_SHAPE: int
    @ivar CONTROL_RULE: Real-time control rule object.
    @type CONTROL_RULE: int
    @ivar POLLUTANT: Pollutant (water quality constituent) object.
    @type POLLUTANT: int
    @ivar LANDUSE: Land use object.
    @type LANDUSE: int
    @ivar CURVE: Hydraulic/hydrologic curve object.
    @type CURVE: int
    @ivar TIMESERIES: Time series object.
    @type TIMESERIES: int
    @ivar TIME_PATTERN: Time pattern object.
    @type TIME_PATTERN: int
    @ivar SYSTEM: System-level pseudo-object (one per simulation).
    @type SYSTEM: int
    """
    RAIN_GAGE = swmm_Object.swmm_GAGE
    SUBCATCHMENT = swmm_Object.swmm_SUBCATCH
    NODE = swmm_Object.swmm_NODE
    LINK = swmm_Object.swmm_LINK
    AQUIFER = swmm_Object.swmm_AQUIFER
    SNOWPACK = swmm_Object.swmm_SNOWPACK
    UNIT_HYDROGRAPH = swmm_Object.swmm_UNIT_HYDROGRAPH
    LID = swmm_Object.swmm_LID
    STREET = swmm_Object.swmm_STREET
    INLET = swmm_Object.swmm_INLET
    TRANSECT = swmm_Object.swmm_TRANSECT
    XSECTION_SHAPE = swmm_Object.smmm_XSECTION_SHAPE
    CONTROL_RULE = swmm_Object.swmm_CONTROL_RULE
    POLLUTANT = swmm_Object.swmm_POLLUTANT
    LANDUSE = swmm_Object.swmm_LANDUSE
    CURVE = swmm_Object.swmm_CURVE
    TIMESERIES = swmm_Object.swmm_TIMESERIES
    TIME_PATTERN = swmm_Object.swmm_TIME_PATTERN
    SYSTEM = swmm_Object.swmm_SYSTEM

class SWMMNodeTypes(Enum):
    """
    Enumeration of SWMM node subtypes.

    @ivar JUNCTION: Junction node — a simple connection point.
    @type JUNCTION: int
    @ivar OUTFALL: Outfall node — a terminal boundary condition node.
    @type OUTFALL: int
    @ivar STORAGE: Storage node — a node with storage volume.
    @type STORAGE: int
    @ivar DIVIDER: Flow divider node — splits flow among two outgoing links.
    @type DIVIDER: int
    """
    JUNCTION = swmm_NodeType.swmm_JUNCTION
    OUTFALL = swmm_NodeType.swmm_OUTFALL
    STORAGE = swmm_NodeType.swmm_STORAGE
    DIVIDER = swmm_NodeType.swmm_DIVIDER

class SWMMRainGageProperties(Enum):
    """
    Enumeration of rain gage properties accessible via the SWMM API.

    @ivar GAGE_TOTAL_PRECIPITATION: Total precipitation depth at the gage (in or mm).
    @type GAGE_TOTAL_PRECIPITATION: int
    @ivar GAGE_RAINFALL: Rainfall intensity at the gage (in/hr or mm/hr).
    @type GAGE_RAINFALL: int
    @ivar GAGE_SNOWFALL: Snowfall intensity at the gage (in/hr or mm/hr).
    @type GAGE_SNOWFALL: int
    """
    GAGE_TOTAL_PRECIPITATION = swmm_GageProperty.swmm_GAGE_TOTAL_PRECIPITATION # Total precipitation
    GAGE_RAINFALL = swmm_GageProperty.swmm_GAGE_RAINFALL # Rainfall
    GAGE_SNOWFALL = swmm_GageProperty.swmm_GAGE_SNOWFALL # Snowfall

class SWMMSubcatchmentProperties(Enum):
    """
    Enumeration of subcatchment properties accessible via the SWMM API.

    @ivar AREA: Subcatchment drainage area (acres or hectares).
    @type AREA: int
    @ivar RAINGAGE: Index of the rain gage assigned to the subcatchment.
    @type RAINGAGE: int
    @ivar RAINFALL: Current rainfall intensity (in/hr or mm/hr).
    @type RAINFALL: int
    @ivar EVAPORATION: Current evaporation rate (in/hr or mm/hr).
    @type EVAPORATION: int
    @ivar INFILTRATION: Current infiltration rate (in/hr or mm/hr).
    @type INFILTRATION: int
    @ivar RUNOFF: Current runoff flow rate (flow units).
    @type RUNOFF: int
    @ivar REPORT_FLAG: Flag controlling whether results are written to the report file.
    @type REPORT_FLAG: int
    @ivar WIDTH: Characteristic width of the subcatchment overland flow path (ft or m).
    @type WIDTH: int
    @ivar SLOPE: Average slope of the subcatchment (fraction).
    @type SLOPE: int
    @ivar CURB_LENGTH: Total curb length in the subcatchment (ft or m).
    @type CURB_LENGTH: int
    @ivar API_RAINFALL: Externally supplied rainfall intensity for API override (in/hr or mm/hr).
    @type API_RAINFALL: int
    @ivar API_SNOWFALL: Externally supplied snowfall intensity for API override (in/hr or mm/hr).
    @type API_SNOWFALL: int
    @ivar POLLUTANT_BUILDUP: Pollutant buildup on the subcatchment surface (mass/area).
    @type POLLUTANT_BUILDUP: int
    @ivar EXTERNAL_POLLUTANT_BUILDUP: Externally added pollutant buildup (mass/area).
    @type EXTERNAL_POLLUTANT_BUILDUP: int
    @ivar POLLUTANT_RUNOFF_CONCENTRATION: Pollutant concentration in runoff (mass/volume).
    @type POLLUTANT_RUNOFF_CONCENTRATION: int
    @ivar POLLUTANT_PONDED_CONCENTRATION: Pollutant concentration in ponded water (mass/volume).
    @type POLLUTANT_PONDED_CONCENTRATION: int
    @ivar POLLUTANT_TOTAL_LOAD: Total pollutant load washed off (mass).
    @type POLLUTANT_TOTAL_LOAD: int
    """
    AREA = swmm_SubcatchProperty.swmm_SUBCATCH_AREA
    RAINGAGE = swmm_SubcatchProperty.swmm_SUBCATCH_RAINGAGE
    RAINFALL = swmm_SubcatchProperty.swmm_SUBCATCH_RAINFALL
    EVAPORATION = swmm_SubcatchProperty.swmm_SUBCATCH_EVAP
    INFILTRATION = swmm_SubcatchProperty.swmm_SUBCATCH_INFIL
    RUNOFF = swmm_SubcatchProperty.swmm_SUBCATCH_RUNOFF
    REPORT_FLAG = swmm_SubcatchProperty.swmm_SUBCATCH_RPTFLAG
    WIDTH = swmm_SubcatchProperty.swmm_SUBCATCH_WIDTH
    SLOPE = swmm_SubcatchProperty.swmm_SUBCATCH_SLOPE
    CURB_LENGTH = swmm_SubcatchProperty.swmm_SUBCATCH_CURB_LENGTH
    API_RAINFALL = swmm_SubcatchProperty.swmm_SUBCATCH_API_RAINFALL
    API_SNOWFALL = swmm_SubcatchProperty.swmm_SUBCATCH_API_SNOWFALL
    POLLUTANT_BUILDUP = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_BUILDUP
    EXTERNAL_POLLUTANT_BUILDUP = swmm_SubcatchProperty.swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP
    POLLUTANT_RUNOFF_CONCENTRATION = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION
    POLLUTANT_PONDED_CONCENTRATION = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION
    POLLUTANT_TOTAL_LOAD = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD

class SWMMNodeProperties(Enum):
    """
    Enumeration of node properties accessible via the SWMM API.

    @ivar TYPE: Node subtype code (see C{SWMMNodeTypes}).
    @type TYPE: int
    @ivar INVERT_ELEVATION: Node invert elevation above datum (ft or m).
    @type INVERT_ELEVATION: int
    @ivar MAX_DEPTH: Maximum node depth (ft or m).
    @type MAX_DEPTH: int
    @ivar DEPTH: Current water depth above invert (ft or m).
    @type DEPTH: int
    @ivar HYDRAULIC_HEAD: Current hydraulic head (ft or m above datum).
    @type HYDRAULIC_HEAD: int
    @ivar VOLUME: Current volume of water stored at the node (ft3 or m3).
    @type VOLUME: int
    @ivar LATERAL_INFLOW: Current lateral inflow rate (flow units).
    @type LATERAL_INFLOW: int
    @ivar TOTAL_INFLOW: Current total inflow rate (flow units).
    @type TOTAL_INFLOW: int
    @ivar FLOODING: Current flooding (overflow) rate (flow units).
    @type FLOODING: int
    @ivar REPORT_FLAG: Flag controlling whether results are written to the report file.
    @type REPORT_FLAG: int
    @ivar SURCHARGE_DEPTH: Current surcharge depth above the node crown (ft or m).
    @type SURCHARGE_DEPTH: int
    @ivar PONDING_AREA: Surface area used for ponded water storage (ft2 or m2).
    @type PONDING_AREA: int
    @ivar INITIAL_DEPTH: Initial water depth at the start of simulation (ft or m).
    @type INITIAL_DEPTH: int
    @ivar POLLUTANT_CONCENTRATION: Pollutant concentration at the node (mass/volume).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_CONCENTRATION: int
    @ivar POLLUTANT_LATERAL_MASS_FLUX: Lateral pollutant mass flux entering the node (mass/time).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_LATERAL_MASS_FLUX: int
    """
    TYPE = swmm_NodeProperty.swmm_NODE_TYPE
    INVERT_ELEVATION = swmm_NodeProperty.swmm_NODE_ELEV
    MAX_DEPTH = swmm_NodeProperty.swmm_NODE_MAXDEPTH
    DEPTH = swmm_NodeProperty.swmm_NODE_DEPTH
    HYDRAULIC_HEAD = swmm_NodeProperty.swmm_NODE_HEAD
    VOLUME = swmm_NodeProperty.swmm_NODE_VOLUME
    LATERAL_INFLOW = swmm_NodeProperty.swmm_NODE_LATFLOW
    TOTAL_INFLOW = swmm_NodeProperty.swmm_NODE_INFLOW
    FLOODING = swmm_NodeProperty.swmm_NODE_OVERFLOW
    REPORT_FLAG = swmm_NodeProperty.swmm_NODE_RPTFLAG
    SURCHARGE_DEPTH = swmm_NodeProperty.swmm_NODE_SURCHARGE_DEPTH
    PONDING_AREA = swmm_NodeProperty.swmm_NODE_PONDED_AREA
    INITIAL_DEPTH = swmm_NodeProperty.swmm_NODE_INITIAL_DEPTH
    POLLUTANT_CONCENTRATION = swmm_NodeProperty.swmm_NODE_POLLUTANT_CONCENTRATION # Pollutant concentration
    POLLUTANT_LATERAL_MASS_FLUX = swmm_NodeProperty.swmm_NODE_POLLUTANT_LATMASS_FLUX # Pollutant inflow concentration

class SWMMLinkProperties(Enum):
    """
    Enumeration of link properties accessible via the SWMM API.

    @ivar TYPE: Link subtype code (see C{SWMMLinkTypes}).
    @type TYPE: int
    @ivar START_NODE: Index of the upstream node.
    @type START_NODE: int
    @ivar END_NODE: Index of the downstream node.
    @type END_NODE: int
    @ivar LENGTH: Link length (ft or m).
    @type LENGTH: int
    @ivar SLOPE: Link slope (ft/ft or m/m).
    @type SLOPE: int
    @ivar FULL_DEPTH: Full (maximum) depth of the link cross-section (ft or m).
    @type FULL_DEPTH: int
    @ivar FULL_FLOW: Flow rate when the link is flowing full (flow units).
    @type FULL_FLOW: int
    @ivar SETTING: Current control setting (dimensionless fraction, 0-1).
    @type SETTING: int
    @ivar TIME_OPEN: Cumulative time the link control has been open (seconds).
    @type TIME_OPEN: int
    @ivar TIME_CLOSED: Cumulative time the link control has been closed (seconds).
    @type TIME_CLOSED: int
    @ivar FLOW: Current flow rate through the link (flow units).
    @type FLOW: int
    @ivar DEPTH: Current flow depth in the link (ft or m).
    @type DEPTH: int
    @ivar VELOCITY: Current flow velocity (ft/s or m/s).
    @type VELOCITY: int
    @ivar TOP_WIDTH: Current water surface top width (ft or m).
    @type TOP_WIDTH: int
    @ivar VOLUME: Current volume of water in the link (ft3 or m3).
    @type VOLUME: int
    @ivar CAPACITY: Current capacity utilisation as a fraction of full flow (0-1).
    @type CAPACITY: int
    @ivar REPORT_FLAG: Flag controlling whether results are written to the report file.
    @type REPORT_FLAG: int
    @ivar START_NODE_OFFSET: Offset of the link invert above the upstream node invert (ft or m).
    @type START_NODE_OFFSET: int
    @ivar END_NODE_OFFSET: Offset of the link invert above the downstream node invert (ft or m).
    @type END_NODE_OFFSET: int
    @ivar INITIAL_FLOW: Initial flow rate at the start of simulation (flow units).
    @type INITIAL_FLOW: int
    @ivar FLOW_LIMIT: Maximum allowable flow rate (flow units).
    @type FLOW_LIMIT: int
    @ivar INLET_LOSS: Minor loss coefficient at the link inlet (dimensionless).
    @type INLET_LOSS: int
    @ivar OUTLET_LOSS: Minor loss coefficient at the link outlet (dimensionless).
    @type OUTLET_LOSS: int
    @ivar AVERAGE_LOSS: Average minor loss coefficient (dimensionless).
    @type AVERAGE_LOSS: int
    @ivar SEEPAGE_RATE: Seepage loss rate per unit length (flow units/length).
    @type SEEPAGE_RATE: int
    @ivar HAS_FLAPGATE: Flag indicating presence of a flap gate (1 = yes, 0 = no).
    @type HAS_FLAPGATE: int
    @ivar POLLUTANT_CONCENTRATION: Pollutant concentration in the link flow (mass/volume).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_CONCENTRATION: int
    @ivar POLLUTANT_LOAD: Pollutant load transported through the link (mass/time).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_LOAD: int
    @ivar POLLUTANT_LATERAL_MASS_FLUX: Lateral pollutant mass flux entering the link (mass/time).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_LATERAL_MASS_FLUX: int
    """
    TYPE = swmm_LinkProperty.swmm_LINK_TYPE
    START_NODE = swmm_LinkProperty.swmm_LINK_NODE1
    END_NODE = swmm_LinkProperty.swmm_LINK_NODE2
    LENGTH = swmm_LinkProperty.swmm_LINK_LENGTH
    SLOPE = swmm_LinkProperty.swmm_LINK_SLOPE
    FULL_DEPTH = swmm_LinkProperty.swmm_LINK_FULLDEPTH
    FULL_FLOW = swmm_LinkProperty.swmm_LINK_FULLFLOW
    SETTING = swmm_LinkProperty.swmm_LINK_SETTING
    TIME_OPEN = swmm_LinkProperty.swmm_LINK_TIMEOPEN
    TIME_CLOSED = swmm_LinkProperty.swmm_LINK_TIMECLOSED
    FLOW = swmm_LinkProperty.swmm_LINK_FLOW
    DEPTH = swmm_LinkProperty.swmm_LINK_DEPTH
    VELOCITY = swmm_LinkProperty.swmm_LINK_VELOCITY
    TOP_WIDTH = swmm_LinkProperty.swmm_LINK_TOPWIDTH
    VOLUME = swmm_LinkProperty.swmm_LINK_VOLUME
    CAPACITY = swmm_LinkProperty.swmm_LINK_CAPACITY
    REPORT_FLAG = swmm_LinkProperty.swmm_LINK_RPTFLAG
    START_NODE_OFFSET = swmm_LinkProperty.swmm_LINK_OFFSET1
    END_NODE_OFFSET = swmm_LinkProperty.swmm_LINK_OFFSET2
    INITIAL_FLOW = swmm_LinkProperty.swmm_LINK_INITIAL_FLOW
    FLOW_LIMIT = swmm_LinkProperty.swmm_LINK_FLOW_LIMIT
    INLET_LOSS = swmm_LinkProperty.swmm_LINK_INLET_LOSS
    OUTLET_LOSS = swmm_LinkProperty.swmm_LINK_OUTLET_LOSS
    AVERAGE_LOSS = swmm_LinkProperty.swmm_LINK_AVERAGE_LOSS
    SEEPAGE_RATE = swmm_LinkProperty.swmm_LINK_SEEPAGE_RATE
    HAS_FLAPGATE = swmm_LinkProperty.swmm_LINK_HAS_FLAPGATE
    POLLUTANT_CONCENTRATION = swmm_LinkProperty.swmm_LINK_POLLUTANT_CONCENTRATION  # Pollutant concentration
    POLLUTANT_LOAD = swmm_LinkProperty.swmm_LINK_POLLUTANT_LOAD # Pollutant load
    POLLUTANT_LATERAL_MASS_FLUX = swmm_LinkProperty.swmm_LINK_POLLUTANT_LATMASS_FLUX # Pollutant lateral mass flux

class SWMMLinkTypes(Enum):
    """
    Enumeration of SWMM link subtypes.

    @ivar CONDUIT: Gravity-flow conduit link.
    @type CONDUIT: int
    @ivar PUMP: Pump link.
    @type PUMP: int
    @ivar ORIFICE: Orifice control link.
    @type ORIFICE: int
    @ivar WEIR: Weir control link.
    @type WEIR: int
    @ivar OUTLET: Outlet control link.
    @type OUTLET: int
    """
    CONDUIT = swmm_LinkType.swmm_CONDUIT
    PUMP = swmm_LinkType.swmm_PUMP
    ORIFICE = swmm_LinkType.swmm_ORIFICE
    WEIR = swmm_LinkType.swmm_WEIR
    OUTLET = swmm_LinkType.swmm_OUTLET

class SWMMSystemProperties(Enum):
    """
    Enumeration of system-level simulation properties accessible via the SWMM API.

    @ivar START_DATE: Simulation start date/time encoded as a SWMM double value.
    @type START_DATE: int
    @ivar CURRENT_DATE: Current simulation date/time encoded as a SWMM double value.
    @type CURRENT_DATE: int
    @ivar ELAPSED_TIME: Elapsed simulation time in decimal days.
    @type ELAPSED_TIME: int
    @ivar ROUTING_STEP: Hydraulic routing time step in seconds.
    @type ROUTING_STEP: int
    @ivar MAX_ROUTING_STEP: Maximum allowable routing time step in seconds.
    @type MAX_ROUTING_STEP: int
    @ivar REPORT_STEP: Reporting time step in seconds.
    @type REPORT_STEP: int
    @ivar TOTAL_STEPS: Total number of routing steps to be performed.
    @type TOTAL_STEPS: int
    @ivar NO_REPORT_FLAG: Flag suppressing output report generation (1 = suppress).
    @type NO_REPORT_FLAG: int
    @ivar FLOW_UNITS: Active flow unit code (see C{SWMMFlowUnits}).
    @type FLOW_UNITS: int
    @ivar END_DATE: Simulation end date/time encoded as a SWMM double value.
    @type END_DATE: int
    @ivar REPORT_START_DATE: Report start date/time encoded as a SWMM double value.
    @type REPORT_START_DATE: int
    @ivar UNIT_SYSTEM: Unit system code (0 = US customary, 1 = SI).
    @type UNIT_SYSTEM: int
    @ivar SURCHARGE_METHOD: Surcharge method code (0 = original SWMM, 1 = EXTRAN).
    @type SURCHARGE_METHOD: int
    @ivar ALLOW_PONDING: Flag enabling surface ponding at flooded nodes (1 = allow).
    @type ALLOW_PONDING: int
    @ivar INTERTIAL_DAMPING: Inertial damping option code.
    @type INTERTIAL_DAMPING: int
    @ivar NORMAL_FLOW_LIMITED: Normal flow limiting option code.
    @type NORMAL_FLOW_LIMITED: int
    @ivar SKIP_STEADY_STATE: Flag enabling steady-state skip detection (1 = skip).
    @type SKIP_STEADY_STATE: int
    @ivar IGNORE_RAINFALL: Flag suppressing rainfall processing (1 = ignore).
    @type IGNORE_RAINFALL: int
    @ivar IGNORE_RDII: Flag suppressing RDII processing (1 = ignore).
    @type IGNORE_RDII: int
    @ivar IGNORE_SNOWMELT: Flag suppressing snowmelt processing (1 = ignore).
    @type IGNORE_SNOWMELT: int
    @ivar IGNORE_GROUNDWATER: Flag suppressing groundwater processing (1 = ignore).
    @type IGNORE_GROUNDWATER: int
    @ivar IGNORE_ROUTING: Flag suppressing hydraulic routing (1 = ignore).
    @type IGNORE_ROUTING: int
    @ivar IGNORE_QUALITY: Flag suppressing water quality simulation (1 = ignore).
    @type IGNORE_QUALITY: int
    @ivar ERROR_CODE: Most recent internal SWMM error code (0 = no error).
    @type ERROR_CODE: int
    @ivar RULE_STEP: Control rule evaluation time step in seconds.
    @type RULE_STEP: int
    @ivar SWEEP_START: Street sweeping season start date (day of year).
    @type SWEEP_START: int
    @ivar SWEEP_END: Street sweeping season end date (day of year).
    @type SWEEP_END: int
    @ivar MAX_TRIALS: Maximum number of trials per routing time step.
    @type MAX_TRIALS: int
    @ivar NUM_THREADS: Number of parallel computation threads.
    @type NUM_THREADS: int
    @ivar MIN_ROUTE_STEP: Minimum allowable routing time step in seconds.
    @type MIN_ROUTE_STEP: int
    @ivar LENGTHENING_STEP: Conduit lengthening time step in seconds.
    @type LENGTHENING_STEP: int
    @ivar START_DRY_DAYS: Number of antecedent dry days before the simulation starts.
    @type START_DRY_DAYS: int
    @ivar COURANT_FACTOR: Courant number factor for time step adjustment.
    @type COURANT_FACTOR: int
    @ivar MIN_SURF_AREA: Minimum node surface area (ft2 or m2).
    @type MIN_SURF_AREA: int
    @ivar MIN_SLOPE: Minimum allowable conduit slope (ft/ft or m/m).
    @type MIN_SLOPE: int
    @ivar RUNOFF_ERROR: Allowable runoff continuity error (fraction).
    @type RUNOFF_ERROR: int
    @ivar FLOW_ERROR: Allowable flow routing continuity error (fraction).
    @type FLOW_ERROR: int
    @ivar QUAL_ERROR: Allowable water quality continuity error (fraction).
    @type QUAL_ERROR: int
    @ivar HEAD_TOL: Convergence tolerance for nodal heads (ft or m).
    @type HEAD_TOL: int
    @ivar SYS_FLOW_TOL: System flow balance tolerance (fraction).
    @type SYS_FLOW_TOL: int
    @ivar LAT_FLOW_TOL: Lateral flow balance tolerance (fraction).
    @type LAT_FLOW_TOL: int
    """
    START_DATE = swmm_SystemProperty.swmm_STARTDATE
    CURRENT_DATE = swmm_SystemProperty.swmm_CURRENTDATE
    ELAPSED_TIME = swmm_SystemProperty.swmm_ELAPSEDTIME
    ROUTING_STEP = swmm_SystemProperty.swmm_ROUTESTEP
    MAX_ROUTING_STEP = swmm_SystemProperty.swmm_MAXROUTESTEP
    REPORT_STEP = swmm_SystemProperty.swmm_REPORTSTEP
    TOTAL_STEPS = swmm_SystemProperty.swmm_TOTALSTEPS
    NO_REPORT_FLAG = swmm_SystemProperty.swmm_NOREPORT
    FLOW_UNITS = swmm_SystemProperty.swmm_FLOWUNITS
    END_DATE = swmm_SystemProperty.swmm_ENDDATE
    REPORT_START_DATE = swmm_SystemProperty.swmm_REPORTSTART
    UNIT_SYSTEM = swmm_SystemProperty.swmm_UNITSYSTEM
    SURCHARGE_METHOD = swmm_SystemProperty.swmm_SURCHARGEMETHOD
    ALLOW_PONDING = swmm_SystemProperty.swmm_ALLOWPONDING
    INTERTIAL_DAMPING = swmm_SystemProperty.swmm_INERTIADAMPING
    NORMAL_FLOW_LIMITED = swmm_SystemProperty.swmm_NORMALFLOWLTD
    SKIP_STEADY_STATE = swmm_SystemProperty.swmm_SKIPSTEADYSTATE
    IGNORE_RAINFALL = swmm_SystemProperty.swmm_IGNORERAINFALL
    IGNORE_RDII = swmm_SystemProperty.swmm_IGNORERDII
    IGNORE_SNOWMELT = swmm_SystemProperty.swmm_IGNORESNOWMELT
    IGNORE_GROUNDWATER = swmm_SystemProperty.swmm_IGNOREGROUNDWATER
    IGNORE_ROUTING = swmm_SystemProperty.swmm_IGNOREROUTING
    IGNORE_QUALITY = swmm_SystemProperty.swmm_IGNOREQUALITY
    ERROR_CODE = swmm_SystemProperty.swmm_ERROR_CODE
    RULE_STEP = swmm_SystemProperty.swmm_RULESTEP
    SWEEP_START = swmm_SystemProperty.swmm_SWEEPSTART
    SWEEP_END = swmm_SystemProperty.swmm_SWEEPEND
    MAX_TRIALS = swmm_SystemProperty.swmm_MAXTRIALS
    NUM_THREADS = swmm_SystemProperty.swmm_NUMTHREADS
    MIN_ROUTE_STEP = swmm_SystemProperty.swmm_MINROUTESTEP
    LENGTHENING_STEP = swmm_SystemProperty.swmm_LENGTHENINGSTEP
    START_DRY_DAYS = swmm_SystemProperty.swmm_STARTDRYDAYS
    COURANT_FACTOR = swmm_SystemProperty.swmm_COURANTFACTOR
    MIN_SURF_AREA = swmm_SystemProperty.swmm_MINSURFAREA
    MIN_SLOPE = swmm_SystemProperty.swmm_MINSLOPE
    RUNOFF_ERROR = swmm_SystemProperty.swmm_RUNOFFERROR
    FLOW_ERROR = swmm_SystemProperty.swmm_FLOWERROR
    QUAL_ERROR = swmm_SystemProperty.swmm_QUALERROR
    HEAD_TOL = swmm_SystemProperty.swmm_HEADTOL
    SYS_FLOW_TOL = swmm_SystemProperty.swmm_SYSFLOWTOL
    LAT_FLOW_TOL = swmm_SystemProperty.swmm_LATFLOWTOL

class SWMMFlowUnits(Enum):
    """
    Enumeration of SWMM flow unit systems.

    @ivar CFS: Cubic feet per second (US customary).
    @type CFS: int
    @ivar GPM: Gallons per minute (US customary).
    @type GPM: int
    @ivar MGD: Million gallons per day (US customary).
    @type MGD: int
    @ivar CMS: Cubic meters per second (SI).
    @type CMS: int
    @ivar LPS: Liters per second (SI).
    @type LPS: int
    @ivar MLD: Million liters per day (SI).
    @type MLD: int
    """
    CFS = swmm_FlowUnitsProperty.swmm_CFS
    GPM = swmm_FlowUnitsProperty.swmm_GPM
    MGD = swmm_FlowUnitsProperty.swmm_MGD
    CMS = swmm_FlowUnitsProperty.swmm_CMS
    LPS = swmm_FlowUnitsProperty.swmm_LPS
    MLD = swmm_FlowUnitsProperty.swmm_MLD

class SWMMAPIErrors(Enum):
    """
    Enumeration of SWMM API error codes returned by C API functions.

    @ivar PROJECT_NOT_OPENED: The SWMM project has not been opened yet.
    @type PROJECT_NOT_OPENED: int
    @ivar SIMULATION_NOT_STARTED: The simulation has not been started yet.
    @type SIMULATION_NOT_STARTED: int
    @ivar SIMULATION_NOT_ENDED: The simulation has not been ended yet.
    @type SIMULATION_NOT_ENDED: int
    @ivar OBJECT_TYPE: Invalid or unrecognised SWMM object type code.
    @type OBJECT_TYPE: int
    @ivar OBJECT_INDEX: Object index is out of range.
    @type OBJECT_INDEX: int
    @ivar OBJECT_NAME: Object name was not found.
    @type OBJECT_NAME: int
    @ivar PROPERTY_TYPE: Invalid or unrecognised property type code.
    @type PROPERTY_TYPE: int
    @ivar PROPERTY_VALUE: Property value is out of the allowable range.
    @type PROPERTY_VALUE: int
    @ivar TIME_PERIOD: Requested time period is invalid.
    @type TIME_PERIOD: int
    @ivar HOTSTART_FILE_OPEN: Error opening the hot-start file.
    @type HOTSTART_FILE_OPEN: int
    @ivar HOTSTART_FILE_FORMAT: Hot-start file has an invalid format.
    @type HOTSTART_FILE_FORMAT: int
    @ivar SIMULATION_IS_RUNNING: Operation not permitted while simulation is running.
    @type SIMULATION_IS_RUNNING: int
    """
    PROJECT_NOT_OPENED = swmm_API_Errors.ERR_API_NOT_OPEN          # API not open
    SIMULATION_NOT_STARTED = swmm_API_Errors.ERR_API_NOT_STARTED       # API not started
    SIMULATION_NOT_ENDED = swmm_API_Errors.ERR_API_NOT_ENDED         # API not ended
    OBJECT_TYPE = swmm_API_Errors.ERR_API_OBJECT_TYPE       # Invalid object type
    OBJECT_INDEX = swmm_API_Errors.ERR_API_OBJECT_INDEX      # Invalid object index
    OBJECT_NAME = swmm_API_Errors.ERR_API_OBJECT_NAME       # Invalid object name
    PROPERTY_TYPE = swmm_API_Errors.ERR_API_PROPERTY_TYPE     # Invalid property type
    PROPERTY_VALUE = swmm_API_Errors.ERR_API_PROPERTY_VALUE    # Invalid property value
    TIME_PERIOD = swmm_API_Errors.ERR_API_TIME_PERIOD       # Invalid time period
    HOTSTART_FILE_OPEN = swmm_API_Errors.ERR_API_HOTSTART_FILE_OPEN # Error opening hotstart file
    HOTSTART_FILE_FORMAT = swmm_API_Errors.ERR_API_HOTSTART_FILE_FORMAT # Invalid hotstart file format
    SIMULATION_IS_RUNNING = swmm_API_Errors.ERR_API_IS_RUNNING # Simulation is running

cdef void c_wrapper_function(double x):
    """
    C-level trampoline that forwards a progress value to the registered Python callable.

    @param x: Progress value (0.0-1.0) forwarded from the C progress callback.
    @type x: double
    """
    global py_progress_callback
    cdef tuple args = (x,)
    PyObject_CallObject(py_progress_callback, args)

cdef progress_callback wrap_python_function_as_callback(object py_func):
    """
    Store a Python callable and return a C function pointer that invokes it.

    @param py_func: Python callable accepting a single C{float} progress argument.
    @type py_func: callable
    @return: C function pointer wrapping C{c_wrapper_function}.
    @rtype: progress_callback
    """
    global py_progress_callback
    py_progress_callback = py_func
    return <progress_callback>c_wrapper_function

cdef object global_solver = None

cdef void progress_callback_wrapper(double progress):
    """
    C-level callback that forwards the progress value to the active C{Solver} instance.

    @param progress: Fractional simulation progress (0.0–1.0).
    @type progress: double
    """
    global solver_instance

    if solver_instance is not None:
        solver_instance.__progress_callback(progress)

def run_solver(
    inp_file: str,
    rpt_file: str = None,
    out_file: str = None,
    swmm_progress_callback: Callable[[float], None] = None
    ) -> int:
    """
    Run a complete SWMM simulation from input file to output file.

    @param inp_file: Path to the SWMM input (``.inp``) file.
    @type inp_file: str
    @param rpt_file: Path for the report (``.rpt``) file.  If C{None}, the
        path is derived by replacing the ``.inp`` extension with ``.rpt``.
    @type rpt_file: str
    @param out_file: Path for the binary output (``.out``) file.  If C{None},
        the path is derived by replacing the ``.inp`` extension with ``.out``.
    @type out_file: str
    @param swmm_progress_callback: Optional callable invoked with a C{float}
        progress value (0.0–1.0) at regular intervals during the run.
    @type swmm_progress_callback: Callable[[float], None]
    @return: SWMM error code; C{0} indicates success.
    @rtype: int
    @raise SWMMSolverException: If the simulation fails with a non-zero error code.
    """
    cdef int error_code = 0
    cdef bytes c_inp_file_bytes = inp_file.encode('utf-8')
    cdef progress_callback c_swm_progress_callback

    if rpt_file is not None:
       rpt_file = inp_file.replace('.inp', '.rpt')

    if out_file is not None:
         out_file = inp_file.replace('.inp', '.out')

    cdef bytes c_rpt_file_bytes = rpt_file.encode('utf-8')
    cdef bytes c_out_file_bytes = out_file.encode('utf-8')

    cdef const char* c_inp_file = c_inp_file_bytes
    cdef const char* c_rpt_file = c_rpt_file_bytes
    cdef const char* c_out_file = c_out_file_bytes

    if swmm_progress_callback is not None:
        c_swm_progress_callback = <progress_callback>wrap_python_function_as_callback(swmm_progress_callback)
        error_code = swmm_run_with_callback(c_inp_file, c_rpt_file, c_out_file, c_swm_progress_callback)
    else:
        error_code = swmm_run(c_inp_file, c_rpt_file, c_out_file)

    if error_code != 0:
        raise SWMMSolverException(f'Run failed with message: {get_error_message(error_code)}')

    return error_code

cpdef cython_datetime decode_swmm_datetime(double swmm_datetime):
    """
    Decode a SWMM double-precision date value into a Python C{datetime} object.

    SWMM stores dates as the number of days elapsed since 30 December 1899
    (i.e., the OLE Automation date format).

    @param swmm_datetime: SWMM double-precision date value.
    @type swmm_datetime: float
    @return: Decoded C{datetime} object.
    @rtype: datetime
    """
    cdef int year, month, day, hour, minute, second, day_of_week
    swmm_decodeDate(swmm_datetime, &year, &month, &day, &hour, &minute, &second, &day_of_week)

    return datetime(year, month, day, hour, minute, second)

cpdef double encode_swmm_datetime(cython_datetime dt):
    """
    Encode a Python C{datetime} object as a SWMM double-precision date value.

    @param dt: The C{datetime} to encode.
    @type dt: datetime
    @return: SWMM double-precision date value (days since 30 December 1899).
    @rtype: float
    """
    cdef int year = dt.year
    cdef int month = dt.month
    cdef int day = dt.day
    cdef int hour = dt.hour
    cdef int minute = dt.minute
    cdef int second = dt.second

    return swmm_encodeDate(year, month, day, hour, minute, second)

cpdef int version():
    """
    Return the SWMM engine version number.

    @return: Integer version number (e.g., C{51017} for SWMM 5.1.017).
    @rtype: int
    """
    cdef int swmm_version = swmm_getVersion()

    return swmm_version

cpdef str get_error_message(int error_code):
    """
    Return the human-readable message for a SWMM integer error code.

    @param error_code: SWMM error code (typically a negative integer or a
        positive API error code).
    @type error_code: int
    @return: Descriptive error message string decoded from UTF-8.
    @rtype: str
    """
    cdef char* c_error_message = <char*>malloc(1024*sizeof(char))

    swmm_getErrorFromCode(error_code, &c_error_message)

    error_message = c_error_message.decode('utf-8')

    free(c_error_message)

    return error_message

class SolverState(Enum):
    """
    Enumeration representing the lifecycle state of a C{Solver} instance.

    States transition in the order CREATED → OPEN → STARTED → FINISHED →
    ENDED → REPORTED → CLOSED, with CLOSED allowing a return to OPEN.

    @ivar CREATED: Solver has been instantiated but C{open()} has not been called.
    @type CREATED: int
    @ivar OPEN: The SWMM project has been opened successfully.
    @type OPEN: int
    @ivar STARTED: The simulation has been started (C{start()} called).
    @type STARTED: int
    @ivar FINISHED: All routing steps have completed (elapsed time reached zero).
    @type FINISHED: int
    @ivar ENDED: The simulation has been ended (C{end()} called).
    @type ENDED: int
    @ivar REPORTED: Results have been written to the report file (C{report()} called).
    @type REPORTED: int
    @ivar CLOSED: The SWMM project has been closed (C{close()} called).
    @type CLOSED: int
    """
    CREATED = 0
    OPEN = 1
    STARTED = 2
    FINISHED = 3
    ENDED = 4
    REPORTED = 5
    CLOSED = 6

class CallbackType(Enum):
    """
    Enumeration of lifecycle events for which callbacks can be registered.

    @ivar BEFORE_INITIALIZE: Fired before C{open()} is called during context-manager entry.
    @type BEFORE_INITIALIZE: int
    @ivar BEFORE_OPEN: Fired immediately before C{swmm_open()} is called.
    @type BEFORE_OPEN: int
    @ivar AFTER_OPEN: Fired immediately after C{swmm_open()} succeeds.
    @type AFTER_OPEN: int
    @ivar BEFORE_START: Fired immediately before C{swmm_start()} is called.
    @type BEFORE_START: int
    @ivar AFTER_START: Fired immediately after C{swmm_start()} succeeds.
    @type AFTER_START: int
    @ivar BEFORE_STEP: Fired before each stride step.
    @type BEFORE_STEP: int
    @ivar AFTER_STEP: Fired after each stride step.
    @type AFTER_STEP: int
    @ivar BEFORE_END: Fired immediately before C{swmm_end()} is called.
    @type BEFORE_END: int
    @ivar AFTER_END: Fired immediately after C{swmm_end()} succeeds.
    @type AFTER_END: int
    @ivar BEFORE_REPORT: Fired immediately before C{swmm_report()} is called.
    @type BEFORE_REPORT: int
    @ivar AFTER_REPORT: Fired immediately after C{swmm_report()} succeeds.
    @type AFTER_REPORT: int
    @ivar BEFORE_CLOSE: Fired immediately before C{swmm_close()} is called.
    @type BEFORE_CLOSE: int
    @ivar AFTER_CLOSE: Fired immediately after C{swmm_close()} succeeds.
    @type AFTER_CLOSE: int
    """
    BEFORE_INITIALIZE = 0
    BEFORE_OPEN = 1
    AFTER_OPEN = 2
    BEFORE_START = 3
    AFTER_START = 4
    BEFORE_STEP = 5
    AFTER_STEP = 6
    BEFORE_END = 7
    AFTER_END = 8
    BEFORE_REPORT = 9
    AFTER_REPORT = 10
    BEFORE_CLOSE = 11
    AFTER_CLOSE = 12

class SWMMSolverException(Exception):
    """
    Exception raised when a SWMM solver operation fails.

    @param message: Human-readable description of the failure.
    @type message: str
    """
    def __init__(self, message: str) -> None:
        """
        Initialise the exception with a descriptive error message.

        @param message: Human-readable description of the failure.
        @type message: str
        """
        super().__init__(message)

cdef class Solver:
    """
    High-level interface to the SWMM engine for step-by-step or batch simulation.

    Wraps the SWMM C API (C{swmm_open}, C{swmm_start}, C{swmm_stride},
    C{swmm_end}, C{swmm_report}, C{swmm_close}) and provides property-based
    access to simulation parameters, lifecycle callbacks, and progress reporting.

    Use as a context manager for automatic lifecycle management::

        with Solver('model.inp') as solver:
            for elapsed, current_dt in solver:
                print(current_dt)

    Or exercise full manual control::

        solver = Solver('model.inp')
        solver.open()
        solver.start()
        while True:
            elapsed, dt = solver.step()
            if elapsed <= 0:
                break
        solver.end()
        solver.report()
        solver.close()
    """
    cdef str _inp_file
    cdef str _rpt_file
    cdef str _out_file
    cdef bint _save_results
    cdef int _stride_step
    cdef dict _callbacks
    cdef int _progress_callbacks_per_second
    cdef list _progress_callbacks
    cdef clock_t _clock
    cdef double _total_duration
    cdef object _solver_state
    cdef object _partial_step_function

    # ====================================================================
    # File lifecycle
    # ====================================================================

    def __cinit__(
        self,
        str inp_file,
        str rpt_file = None,
        str out_file = None,
        int stride_step = 300,
        bint save_results=True
    ):
        """
        Construct a new C{Solver} instance without opening the SWMM project.

        The SWMM project is not opened until C{open()} or C{initialize()} is
        called (or the instance is used as a context manager).

        @param inp_file: Path to the SWMM input (``.inp``) file.
        @type inp_file: str
        @param rpt_file: Path for the report (``.rpt``) file.  If C{None},
            derived from C{inp_file} by substituting the ``.inp`` extension.
        @type rpt_file: str
        @param out_file: Path for the binary output (``.out``) file.  If C{None},
            derived from C{inp_file} by substituting the ``.inp`` extension.
        @type out_file: str
        @param stride_step: Default stride duration in seconds for each
            C{step()} call.  Defaults to C{300}.
        @type stride_step: int
        @param save_results: Whether to save results to the binary output file.
            Defaults to C{True}.
        @type save_results: bool
        """
        global global_solver
        self._save_results = save_results
        self._inp_file = inp_file
        self._progress_callbacks_per_second = 2
        self._stride_step = stride_step
        self._clock = clock()
        global_solver = self

        if rpt_file is not None:
            self._rpt_file = rpt_file
        else:
            self._rpt_file = inp_file.replace('.inp', '.rpt')

        if out_file is not None:
            self._out_file = out_file
        else:
            self._out_file = inp_file.replace('.inp', '.out')

        self._callbacks = {
            CallbackType.BEFORE_INITIALIZE: [],
            CallbackType.BEFORE_OPEN: [],
            CallbackType.AFTER_OPEN: [],
            CallbackType.BEFORE_START: [],
            CallbackType.AFTER_START: [],
            CallbackType.BEFORE_STEP: [],
            CallbackType.AFTER_STEP: [],
            CallbackType.BEFORE_END: [],
            CallbackType.AFTER_END: [],
            CallbackType.BEFORE_REPORT: [],
            CallbackType.AFTER_REPORT: [],
            CallbackType.BEFORE_CLOSE: [],
            CallbackType.AFTER_CLOSE: []
        }

        self._progress_callbacks = []

        self._solver_state = SolverState.CREATED

    def __enter__(self):
        """
        Enter the runtime context, opening the SWMM project.

        Executes C{BEFORE_INITIALIZE} callbacks then calls C{open()}.

        @return: This C{Solver} instance.
        @rtype: Solver
        """
        self.__execute_callbacks(CallbackType.BEFORE_INITIALIZE)
        self.open()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        """
        Exit the runtime context, finalising the simulation and releasing resources.

        Calls C{finalize()} regardless of whether an exception was raised.

        @param exc_type: Exception type, or C{None} if no exception was raised.
        @param exc_value: Exception instance, or C{None}.
        @param traceback: Traceback object, or C{None}.
        """
        self.finalize()

    def __dealloc__(self):
        """
        Cython destructor — ensure all SWMM resources are released.

        Calls C{finalize()} to cleanly end, report, and close the simulation
        if it has not already been closed, preventing resource leaks.
        """
        self.finalize()

    def __iter__(self):
        """
        Return this C{Solver} as its own iterator for use in C{for} loops.

        @return: This C{Solver} instance.
        @rtype: Solver
        """
        return self

    def __next__(self):
        """
        Advance the simulation by one stride step and return timing information.

        Raises C{StopIteration} when the simulation state has reached
        C{SolverState.FINISHED} (elapsed time returned by C{step()} is zero).

        @return: 2-tuple of (elapsed simulation time in seconds, current datetime).
        @rtype: Tuple[float, datetime]
        @raise StopIteration: When C{solver_state} is C{FINISHED}.
        @raise SWMMSolverException: If the underlying stride call fails.
        """
        if self._solver_state == SolverState.FINISHED:
            raise StopIteration
        else:
            return self.step()

    # ====================================================================
    # Timing
    # ====================================================================

    @property
    def start_datetime(self) -> datetime:
        """
        Simulation start date and time.

        @return: Start date/time decoded from the SWMM C{START_DATE} system property.
        @rtype: datetime
        """
        cdef double start_date = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.START_DATE.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return decode_swmm_datetime(start_date)

    @start_datetime.setter
    def start_datetime(self, sim_start_datetime: datetime) -> None:
        """
        Set the simulation start date and time.

        @param sim_start_datetime: New start date/time for the simulation.
        @type sim_start_datetime: datetime
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef double start_date = encode_swmm_datetime(dt=sim_start_datetime)
        cdef int error_code = swmm_setValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.START_DATE.value,
            index=0,
            subindex=0,
            pollutantIndex=0,
            value=start_date
        )

        self.__validate_error(error_code)

    @property
    def end_datetime(self) -> datetime:
        """
        Simulation end date and time.

        @return: End date/time decoded from the SWMM C{END_DATE} system property.
        @rtype: datetime
        """
        cdef double end_date = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.END_DATE.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return decode_swmm_datetime(end_date)

    @end_datetime.setter
    def end_datetime(self, sim_end_datetime: datetime) -> None:
        """
        Set the simulation end date and time.

        @param sim_end_datetime: New end date/time for the simulation.
        @type sim_end_datetime: datetime
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef double end_date = encode_swmm_datetime(dt=sim_end_datetime)
        cdef int error_code = swmm_setValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.END_DATE.value,
            index=0,
            subindex=0,
            value=end_date,
            pollutantIndex=0,
        )

        self.__validate_error(error_code)

    @property
    def routing_step(self) -> float:
        """
        Hydraulic routing time step in seconds.

        @return: Current routing time step in seconds.
        @rtype: float
        """
        cdef double routing_step = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.ROUTING_STEP.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return routing_step

    @routing_step.setter
    def routing_step(self, value: float) -> None:
        """
        Set the hydraulic routing time step.

        @param value: New routing time step in seconds.
        @type value: float
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef int error_code = swmm_setValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.ROUTING_STEP.value,
            index=0,
            subindex=0,
            pollutantIndex=0,
            value=value
        )

        self.__validate_error(error_code)

    @property
    def reporting_step(self) -> float:
        """
        Reporting time step in seconds.

        @return: Current reporting time step in seconds.
        @rtype: float
        """
        cdef double reporting_step = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.REPORT_STEP.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return reporting_step

    @reporting_step.setter
    def reporting_step(self, value: float) -> None:
        """
        Set the reporting time step.

        @param value: New reporting time step in seconds.
        @type value: float
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef int error_code = swmm_setValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.REPORT_STEP.value,
            index=0,
            subindex=0,
            pollutantIndex=0,
            value=value
        )

        self.__validate_error(error_code)

    @property
    def report_start_datetime(self) -> datetime:
        """
        Report start date and time (results before this date are not written).

        @return: Report start date/time decoded from the SWMM system property.
        @rtype: datetime
        """
        cdef double report_start_date = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.REPORT_START_DATE.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return decode_swmm_datetime(report_start_date)

    @report_start_datetime.setter
    def report_start_datetime(self, report_start_datetime: datetime) -> None:
        """
        Set the report start date and time.

        @param report_start_datetime: New report start date/time.
        @type report_start_datetime: datetime
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef double report_start_date = encode_swmm_datetime(report_start_datetime)
        cdef int error_code = swmm_setValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.REPORT_START_DATE.value,
            index=0,
            subindex=0,
            pollutantIndex=0,
            value=report_start_date
        )

        self.__validate_error(error_code)

    @property
    def current_datetime(self) -> datetime:
        """
        Current simulation date and time (read-only during a running simulation).

        @return: Current simulation date/time decoded from the SWMM system property.
        @rtype: datetime
        """
        cdef double current_date = swmm_getValueExpanded(
            objType=SWMMObjects.SYSTEM.value,
            property=SWMMSystemProperties.CURRENT_DATE.value,
            index=0,
            subIndex=0,
            pollutantIndex=0
        )

        return decode_swmm_datetime(current_date)

    @property
    def stride_step(self) -> int:
        """
        Default stride duration used by C{step()} in seconds.

        @return: Current stride step value in seconds.
        @rtype: int
        """
        return self._stride_step

    @stride_step.setter
    def stride_step(self, value: int):
        """
        Set the default stride duration used by C{step()}.

        @param value: New stride step value in seconds.
        @type value: int
        """
        self._stride_step = value

    # ====================================================================
    # Callbacks / diagnostics
    # ====================================================================

    @property
    def progress_callbacks_per_second(self) -> int:
        """
        Maximum number of times per wall-clock second that progress callbacks
        are invoked during C{execute()}.

        @return: Current progress callback rate (callbacks per second).
        @rtype: int
        """
        return self._progress_callbacks_per_second

    @progress_callbacks_per_second.setter
    def progress_callbacks_per_second(self, value: int) -> None:
        """
        Set the progress callback rate.

        Values less than C{1} are silently clamped to C{1}.

        @param value: Desired callback invocations per wall-clock second.
        @type value: int
        """
        self._progress_callbacks_per_second = max(1, value)

    @property
    def solver_state(self) -> SolverState:
        """
        Current lifecycle state of the solver.

        @return: A C{SolverState} member reflecting the current state.
        @rtype: SolverState
        """
        return self._solver_state

    def add_callback(self, callback_type: CallbackType, callback: Callable[[Solver], None]) -> None:
        """
        Register a lifecycle callback for the given event type.

        The callback receives this C{Solver} instance as its sole argument and
        is called synchronously at the corresponding lifecycle event.

        @param callback_type: The lifecycle event to subscribe to.
        @type callback_type: CallbackType
        @param callback: A callable accepting a single C{Solver} argument.
        @type callback: Callable[[Solver], None]
        """
        self._callbacks[callback_type].append(callback)

    def add_progress_callback(self, callback: Callable[[float], None]) -> None:
        """
        Register a progress callback invoked with simulation progress during C{execute()}.

        @param callback: A callable that accepts a single C{float} progress
            value in the range 0.0 (start) to 1.0 (completion).
        @type callback: Callable[[float], None]
        """
        self._progress_callbacks.append(callback)

    # ====================================================================
    # Per-element accessors
    # ====================================================================

    def get_object_count(self, object_type: SWMMObjects) -> int:
        """
        Return the total count of objects of a given SWMM type.

        @param object_type: The SWMM object category to count.
        @type object_type: SWMMObjects
        @return: Number of objects of C{object_type} in the model.
        @rtype: int
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef int count = swmm_getCount(objType=object_type.value)

        self.__validate_error(count)

        return count

    def get_object_name(self, object_type: SWMMObjects, index: int) -> str:
        """
        Return the name of a single SWMM object.

        @param object_type: The SWMM object category.
        @type object_type: SWMMObjects
        @param index: Zero-based index of the object within its category.
        @type index: int
        @return: Object name string as defined in the input file.
        @rtype: str
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef char* c_object_name = <char*>malloc(1024*sizeof(char))

        cdef int error_code = swmm_getName(
            objType=object_type.value,
            index=index,
            name=c_object_name,
            size=1024
        )

        self.__validate_error(error_code)

        object_name = c_object_name.decode('utf-8')

        free(c_object_name)

        return object_name

    def get_object_names(self, object_type: SWMMObjects) -> List[str]:
        """
        Return the names of all objects of a given SWMM type.

        @param object_type: The SWMM object category.
        @type object_type: SWMMObjects
        @return: Ordered list of object name strings (index matches object index).
        @rtype: List[str]
        @raise SWMMSolverException: If any underlying C API call fails.
        """
        cdef char* c_object_name = <char*>malloc(1024*sizeof(char))
        cdef list object_names = []
        cdef int count = self.get_object_count(object_type=object_type)

        for i in range(count):

            error_code = swmm_getName(
                objType=object_type.value,
                index=i,
                name=c_object_name,
                size=1024
            )

            self.__validate_error(error_code)

            object_name = c_object_name.decode('utf-8')
            object_names.append(object_name)


        free(c_object_name)

        return object_names

    def get_object_index(self, object_type: SWMMObjects, object_name: str) -> int:
        """
        Return the integer index of a named SWMM object.

        @param object_type: The SWMM object category.
        @type object_type: SWMMObjects
        @param object_name: Name of the object to look up.
        @type object_name: str
        @return: Zero-based index of the named object, or C{-1} if not found.
        @rtype: int
        """
        cdef int index = swmm_getIndex(
            objType=object_type.value,
            name=object_name.encode('utf-8')
        )

        return index

    def set_value(
        self,
        object_type: SWMMObjects,
        property_type: Union[
            SWMMRainGageProperties,
            SWMMSubcatchmentProperties,
            SWMMNodeProperties,
            SWMMLinkProperties,
            SWMMSystemProperties
        ],
        index: Union[int, str],
        value: float,
        sub_index: int = -1,
        pollutant_index: int = -1
        ) -> None:
        """
        Set a property value on a SWMM object.

        @param object_type: The SWMM object category (e.g., C{SWMMObjects.NODE}).
        @type object_type: SWMMObjects
        @param property_type: The property to set (e.g., C{SWMMNodeProperties.DEPTH}).
        @type property_type: Union[SWMMRainGageProperties, SWMMSubcatchmentProperties, SWMMNodeProperties, SWMMLinkProperties, SWMMSystemProperties]
        @param index: Zero-based object index or object name string.
        @type index: Union[int, str]
        @param value: New value for the property in the active unit system.
        @type value: float
        @param sub_index: Secondary index for vector properties (e.g., pollutant
            index for C{POLLUTANT_CONCENTRATION}).  Defaults to C{-1} (not used).
        @type sub_index: int
        @param pollutant_index: Pollutant index for water quality properties.
            Defaults to C{-1} (not used).
        @type pollutant_index: int
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef int element_index = -1

        if isinstance(index, str):
            element_index = self.get_object_index(object_type, index)
            self.__validate_error(element_index)
        else:
            element_index = index

        cdef int error_code = swmm_setValueExpanded(
            objType=<int>object_type.value,
            property=<int>property_type.value,
            index=element_index,
            subindex=sub_index,
            pollutantIndex=<int>pollutant_index,
            value=value
        )

        self.__validate_error(error_code)

    def get_value(
        self,
        object_type: SWMMObjects,
        property_type: Union[
            SWMMRainGageProperties,
            SWMMSubcatchmentProperties,
            SWMMNodeProperties,
            SWMMLinkProperties,
            SWMMSystemProperties
        ],
        index: Union[int, str],
        sub_index: int = -1,
        pollutant_index: int = -1
        ) -> float:
        """
        Get a property value from a SWMM object.

        @param object_type: The SWMM object category (e.g., C{SWMMObjects.NODE}).
        @type object_type: SWMMObjects
        @param property_type: The property to retrieve (e.g., C{SWMMNodeProperties.DEPTH}).
        @type property_type: Union[SWMMRainGageProperties, SWMMSubcatchmentProperties, SWMMNodeProperties, SWMMLinkProperties, SWMMSystemProperties]
        @param index: Zero-based object index or object name string.
        @type index: Union[int, str]
        @param sub_index: Secondary index for vector properties (e.g., pollutant
            index for C{POLLUTANT_CONCENTRATION}).  Defaults to C{-1} (not used).
        @type sub_index: int
        @param pollutant_index: Pollutant index for water quality properties.
            Defaults to C{-1} (not used).
        @type pollutant_index: int
        @return: Property value in the active unit system.
        @rtype: float
        @raise SWMMSolverException: If the underlying C API call fails.
        """

        cdef int element_index = -1

        if isinstance(index, str):
            element_index = self.get_object_index(object_type, index)
            self.__validate_error(element_index)
        else:
            element_index = index

        cdef double value = swmm_getValueExpanded(
            objType=<int>object_type.value,
            property=<int>property_type.value,
            index=element_index,
            subIndex=sub_index,
            pollutantIndex=pollutant_index
        )

        self.__validate_error(<int>value)

        return value

    # ====================================================================
    # Simulation control
    # ====================================================================

    cpdef void open(self):
        """
        Open the SWMM project files and transition to the C{OPEN} state.

        Calls C{BEFORE_OPEN} callbacks, invokes C{swmm_open()}, then calls
        C{AFTER_OPEN} callbacks.  Has no effect if already in the C{OPEN} state.

        @raise SWMMSolverException: If the solver is not in a valid state or
            C{swmm_open()} returns a non-zero error code.
        """
        cdef int error_code = 0
        cdef bytes c_inp_file_bytes = self._inp_file.encode('utf-8')
        cdef bytes c_rpt_file_bytes = self._rpt_file.encode('utf-8')
        cdef bytes c_out_file_bytes = self._out_file.encode('utf-8')

        cdef const char* c_inp_file = c_inp_file_bytes
        cdef const char* c_rpt_file = c_rpt_file_bytes
        cdef const char* c_out_file = c_out_file_bytes

        if self._solver_state == SolverState.OPEN:
            pass
        elif self._solver_state == SolverState.CREATED or self._solver_state == SolverState.CLOSED:
            self.__execute_callbacks(CallbackType.BEFORE_OPEN)

            error_code = swmm_open(
                inp_file=c_inp_file,
                rpt_file=c_rpt_file,
                out_file=c_out_file
            )

            self.__validate_error(error_code)
            self._solver_state = SolverState.OPEN
            self.__execute_callbacks(CallbackType.AFTER_OPEN)
            self._clock = clock()
        else:
            raise SWMMSolverException(f'Open failed: Solver is not in a valid state: {self._solver_state}')

        self._total_duration = swmm_getValue(
            property=SWMMSystemProperties.END_DATE.value,
            index=0
        ) - swmm_getValue(
            property=SWMMSystemProperties.START_DATE.value,
            index=0
        )

    cpdef void start(self):
        """
        Start the simulation, transitioning from C{OPEN} to C{STARTED}.

        Calls C{BEFORE_START} callbacks, invokes C{swmm_start()}, then calls
        C{AFTER_START} callbacks.

        @raise SWMMSolverException: If the solver is not in the C{OPEN} state or
            C{swmm_start()} returns a non-zero error code.
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.STARTED:
            pass
        elif self._solver_state == SolverState.OPEN:
            self.__execute_callbacks(CallbackType.BEFORE_START)
            error_code = swmm_start(save_flag=self._save_results)
            self.__validate_error(error_code)
            self._solver_state = SolverState.STARTED
            self.__execute_callbacks(CallbackType.AFTER_START)
        else:
            raise SWMMSolverException(f'Start failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void initialize(self):
        """
        Open and start the simulation in a single call.

        Equivalent to calling C{open()} followed by C{start()}.  Also fires
        the C{BEFORE_INITIALIZE} callbacks before opening.

        @raise SWMMSolverException: If either C{open()} or C{start()} fails.
        """
        self.__execute_callbacks(CallbackType.BEFORE_INITIALIZE)
        self.open()
        self.start()

    cpdef tuple step(self, int steps = 0):
        """
        Advance the simulation by a fixed number of seconds (a "stride step").

        @param steps: Number of seconds to advance.  If greater than C{0},
            overrides the instance C{stride_step} for this call.  Defaults
            to C{0} (use the instance C{stride_step}).
        @type steps: int
        @return: 2-tuple of (elapsed simulation time remaining in seconds —
            C{0.0} when the simulation is complete, current simulation datetime).
        @rtype: Tuple[float, datetime]
        @raise SWMMSolverException: If C{swmm_stride()} returns a negative error code.
        """
        cdef double elapsed_time = 0.0
        cdef double progress = 0.0

        error_code = swmm_stride(strideStep=steps if steps > 0 else self._stride_step, elapsedTime=&elapsed_time)

        if error_code < 0:
            self.__validate_error(error_code)

        progress = (
            swmm_getValue(
                property=SWMMSystemProperties.CURRENT_DATE.value,
                index=0
            ) - swmm_getValue(
                property=SWMMSystemProperties.START_DATE.value,
                index=0
            )
        ) / self._total_duration

        self.__execute_progress_callbacks(progress)

        if elapsed_time <= 0.0:
            self._solver_state = SolverState.FINISHED

        return elapsed_time, decode_swmm_datetime(
            swmm_datetime=swmm_getValue(
                property=SWMMSystemProperties.CURRENT_DATE.value,
                index=0
            )
        )
        # else:
            # raise SWMMSolverException(f'Step failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void end(self):
        """
        End the simulation, transitioning to the C{ENDED} state.

        Calls C{BEFORE_END} callbacks, invokes C{swmm_end()}, then calls
        C{AFTER_END} callbacks.

        @raise SWMMSolverException: If the solver is not in a valid state or
            C{swmm_end()} returns a non-zero error code.
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.ENDED or \
           self._solver_state == SolverState.CREATED:
            pass
        elif self._solver_state == SolverState.OPEN or \
             self._solver_state == SolverState.STARTED or \
             self._solver_state == SolverState.FINISHED:

            self.__execute_callbacks(CallbackType.BEFORE_END)
            error_code = swmm_end()
            self.__validate_error(error_code)
            self._solver_state = SolverState.ENDED
            self.__execute_callbacks(CallbackType.AFTER_END)
        else:
            raise SWMMSolverException(f'End failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void report(self):
        """
        Write simulation results to the report file, transitioning to C{REPORTED}.

        Calls C{BEFORE_REPORT} callbacks, invokes C{swmm_report()}, then calls
        C{AFTER_REPORT} callbacks.

        @raise SWMMSolverException: If the solver is not in the C{ENDED} state or
            C{swmm_report()} returns a non-zero error code.
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.REPORTED or self._solver_state == SolverState.CREATED:
            pass
        elif self._solver_state == SolverState.ENDED:
            self.__execute_callbacks(CallbackType.BEFORE_REPORT)
            error_code = swmm_report()
            self.__validate_error(error_code)
            self._solver_state = SolverState.REPORTED
            self.__execute_callbacks(CallbackType.AFTER_REPORT)
        else:
            raise SWMMSolverException(f'Report failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void close(self):
        """
        Close the SWMM project files, transitioning to the C{CLOSED} state.

        Calls C{BEFORE_CLOSE} callbacks, invokes C{swmm_close()}, then calls
        C{AFTER_CLOSE} callbacks.

        @raise SWMMSolverException: If the solver is not in the C{REPORTED} state
            or C{swmm_close()} returns a non-zero error code.
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.CREATED:
            pass
        elif self._solver_state == SolverState.REPORTED:
            self.__execute_callbacks(CallbackType.BEFORE_CLOSE)
            error_code = swmm_close()
            self.__validate_error(error_code)
            self._solver_state = SolverState.CLOSED
            self.__execute_callbacks(CallbackType.AFTER_CLOSE)
        else:
            raise SWMMSolverException(f'Close failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void finalize(self):
        """
        End, report, and close the simulation in a single call.

        Safely handles all intermediate states: calls C{end()}, C{report()}, and
        C{close()} as required based on the current C{solver_state}.  Has no effect
        if already in the C{CREATED} or C{CLOSED} state.

        @raise SWMMSolverException: If any underlying lifecycle call fails.
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.OPEN or \
           self._solver_state == SolverState.STARTED or \
           self._solver_state == SolverState.FINISHED:

            self.end()
            self.report()
            self.close()
        elif self._solver_state == SolverState.ENDED:
            self.report()
            self.close()
        elif self._solver_state == SolverState.REPORTED:
            self.close()
        elif self._solver_state == SolverState.CREATED or self._solver_state == SolverState.CLOSED:
            pass
        else:
            raise SWMMSolverException(f'Finalize failed: Solver is not in a valid state: {self._solver_state}')

    cpdef void execute(self):
        """
        Run the simulation to completion using the SWMM batch run API.

        Uses C{swmm_run_with_callback} if progress callbacks have been registered,
        otherwise uses C{swmm_run}.  Does not use the step-by-step lifecycle
        (C{open}/C{start}/C{step}/C{end}/C{report}/C{close}).

        @raise SWMMSolverException: If the solver is not in a valid pre-run state
            or the run call fails.
        """
        cdef int error_code = 0
        cdef progress_callback swmm_progress_callback = <progress_callback>progress_callback_wrapper
        cdef bytes c_inp_file_bytes = self._inp_file.encode('utf-8')
        cdef bytes c_rpt_file_bytes = self._rpt_file.encode('utf-8')
        cdef bytes c_out_file_bytes = self._out_file.encode('utf-8')

        cdef const char* c_inp_file = c_inp_file_bytes
        cdef const char* c_rpt_file = c_rpt_file_bytes
        cdef const char* c_out_file = c_out_file_bytes

        if (self._solver_state != SolverState.CREATED or self._solver_state != SolverState.CLOSED):
            if len(self._progress_callbacks) > 0:
                error_code = swmm_run_with_callback(
                    inp_file=c_inp_file,
                    rpt_file=c_rpt_file,
                    out_file=c_out_file,
                    progress=swmm_progress_callback
                )
            else:
                error_code = swmm_run(
                    inp_file=c_inp_file,
                    rpt_file=c_rpt_file,
                    out_file=c_out_file
                )
        else:
            raise SWMMSolverException(f'Solver is not in a valid state: {self._solver_state}')

    # ====================================================================
    # Hot start
    # ====================================================================

    cpdef void use_hotstart(self, str hotstart_file):
        """
        Initialise the simulation state from a previously saved hot-start file.

        Must be called after C{open()} and before C{start()}.

        @param hotstart_file: Path to the hot-start (``.hst``) file to load.
        @type hotstart_file: str
        @raise SWMMSolverException: If C{swmm_useHotStart()} returns a non-zero
            error code.
        """
        cdef bytes c_hotstart_file = hotstart_file.encode('utf-8')
        cdef const char* cc_hotstart_file = c_hotstart_file
        cdef int error_code = swmm_useHotStart(hotStartFile=cc_hotstart_file)

        self.__validate_error(error_code)

    cpdef void save_hotstart(self, str hotstart_file):
        """
        Save the current simulation state to a hot-start file.

        @param hotstart_file: Path where the hot-start (``.hst``) file will be written.
        @type hotstart_file: str
        @raise SWMMSolverException: If C{swmm_saveHotStart()} returns a non-zero
            error code.
        """
        cdef bytes c_hotstart_file = hotstart_file.encode('utf-8')
        cdef const char* cc_hotstart_file = c_hotstart_file
        cdef int error_code = swmm_saveHotStart(hotStartFile=cc_hotstart_file)

        self.__validate_error(error_code)

    # ====================================================================
    # Statistics / mass balance
    # ====================================================================

    def get_mass_balance_error(self) -> Tuple[float, float, float]:
        """
        Return the continuity error percentages for the completed simulation.

        Should be called after C{end()} and before C{close()}.

        @return: 3-tuple of (runoff continuity error %, flow routing continuity
            error %, water quality continuity error %).
        @rtype: Tuple[float, float, float]
        @raise SWMMSolverException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float runoffErr, flowErr, qualErr

        swmm_getMassBalErr(
            runoffErr=&runoffErr,
            flowErr=&flowErr,
            qualErr=&qualErr
        )

        self.__validate_error(error_code)

    # ====================================================================
    # Internal helpers (not part of the public API)
    # ====================================================================

    def __execute_callbacks(self, callback_type: CallbackType) -> None:
        """
        Invoke all registered callbacks for the given lifecycle event type.

        Each callback receives this C{Solver} instance as its sole argument.

        @param callback_type: The lifecycle event whose callbacks should be run.
        @type callback_type: CallbackType
        """
        for callback in self._callbacks[callback_type]:
            callback(self)

    cpdef void __execute_progress_callbacks(self, double percent_complete):
        """
        Invoke all registered progress callbacks with the current completion fraction.

        @param percent_complete: Fractional simulation progress (0.0–1.0).
        @type percent_complete: float
        """
        for callback in self._progress_callbacks:
            callback(percent_complete)

    cdef void __progress_callback(self, double percent_complete):
        """
        C-level progress gate that enforces the configured callbacks-per-second rate.

        Only forwards C{percent_complete} to the registered progress callbacks when
        sufficient wall-clock time has elapsed since the last invocation.

        @param percent_complete: Fractional simulation progress (0.0–1.0).
        @type percent_complete: float
        """
        cdef clock_t elapsed_time =   clock() - self._clock

        if elapsed_time > 1.0 / self._progress_callbacks_per_second:
            self.__execute_progress_callbacks(
                percent_complete=percent_complete
            )

            self._clock = clock()

    cdef void __validate_error(self, error_code: int):
        """
        Check a C API return code and raise C{SWMMSolverException} if negative.

        Also reads the internal SWMM error code via the system property and
        includes it in the exception message when available.

        @param error_code: Integer return value from a C API call.
        @type error_code: int
        @raise SWMMSolverException: If C{error_code} is negative.
        """
        cdef int internal_error_code = <int>swmm_getValue(
            property=SWMMObjects.SYSTEM.value,
            index=SWMMSystemProperties.ERROR_CODE.value
        )

        if error_code < 0:
            if internal_error_code > 0:
                raise SWMMSolverException(f'SWMM failed with message: {internal_error_code}, {self.__get_error()}')
            else:
                raise SWMMSolverException(f'SWMM failed with message: {error_code}, {get_error_message(error_code)}')

    cdef str __get_error(self):
        """
        Retrieve the current error message from the SWMM engine.

        @return: Human-readable error message string decoded from UTF-8.
        @rtype: str
        """
        cdef char* c_error_message = <char*>malloc(1024*sizeof(char))
        swmm_getError(c_error_message, 1024)

        error_message = c_error_message.decode('utf-8')

        free(c_error_message)

        return error_message
