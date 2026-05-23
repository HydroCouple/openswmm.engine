# cython: language_level=3str
"""
openswmm.solver._solver
========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Cython module for the legacy SWMM 5.x solver bindings. Provides
enumerations identifying SWMM objects, an exception class, free helper
functions for running the solver and encoding/decoding SWMM datetimes,
and the L{Solver} class implementing the open / start / step / end /
report / close lifecycle with callback hooks.
"""
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
    swmm_encodeDate,
    swmm_SubcatchStats,
    swmm_NodeStats,
    swmm_StorageStats,
    swmm_OutfallStats,
    swmm_LinkStats,
    swmm_PumpStats,
    swmm_RoutingTotals,
    swmm_RunoffTotals,
    swmm_getSubcatchStats,
    swmm_getNodeStats,
    swmm_getStorageStats,
    swmm_getOutfallStats,
    swmm_getLinkStats,
    swmm_getPumpStats,
    swmm_getSystemRoutingTotals,
    swmm_getSystemRunoffTotals
)

# =============================================================================
# Object / type enumerations
# =============================================================================

class SWMMObjects(Enum):
    """Enumeration of SWMM objects.

    @cvar RAIN_GAGE: Raingage object.
    @cvar SUBCATCHMENT: Subcatchment object.
    @cvar NODE: Node object.
    @cvar LINK: Link object.
    @cvar AQUIFER: Aquifer object.
    @cvar SNOWPACK: Snowpack object.
    @cvar UNIT_HYDROGRAPH: Unit hydrograph object.
    @cvar LID: LID object.
    @cvar STREET: Street object.
    @cvar INLET: Inlet object.
    @cvar TRANSECT: Transect object.
    @cvar XSECTION_SHAPE: Cross-section shape object.
    @cvar CONTROL_RULE: Control rule object.
    @cvar POLLUTANT: Pollutant object.
    @cvar LANDUSE: Land use object.
    @cvar CURVE: Curve object.
    @cvar TIMESERIES: Time series object.
    @cvar TIME_PATTERN: Time pattern object.
    @cvar SYSTEM: System object.
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
    """Enumeration of SWMM node types.

    @cvar JUNCTION: Junction node.
    @cvar OUTFALL: Outfall node.
    @cvar STORAGE: Storage node.
    @cvar DIVIDER: Divider node.
    """
    JUNCTION = swmm_NodeType.swmm_JUNCTION
    OUTFALL = swmm_NodeType.swmm_OUTFALL
    STORAGE = swmm_NodeType.swmm_STORAGE
    DIVIDER = swmm_NodeType.swmm_DIVIDER

# =============================================================================
# Property enumerations
# =============================================================================

class SWMMRainGageProperties(Enum):
    """Enumeration of SWMM raingage properties.

    @cvar GAGE_TOTAL_PRECIPITATION: Total precipitation.
    @cvar GAGE_RAINFALL: Rainfall.
    @cvar GAGE_SNOWFALL: Snowfall.
    """
    GAGE_TOTAL_PRECIPITATION = swmm_GageProperty.swmm_GAGE_TOTAL_PRECIPITATION # Total precipitation
    GAGE_RAINFALL = swmm_GageProperty.swmm_GAGE_RAINFALL # Rainfall
    GAGE_SNOWFALL = swmm_GageProperty.swmm_GAGE_SNOWFALL # Snowfall
    
class SWMMSubcatchmentProperties(Enum):
    """Enumeration of SWMM subcatchment properties.

    @cvar AREA: Area.
    @cvar RAINGAGE: Raingage index.
    @cvar RAINFALL: Rainfall.
    @cvar EVAPORATION: Evaporation.
    @cvar INFILTRATION: Infiltration.
    @cvar RUNOFF: Runoff.
    @cvar REPORT_FLAG: Report flag.
    @cvar WIDTH: Width.
    @cvar SLOPE: Slope.
    @cvar OUTLET_TYPE: Outlet object type (node or subcatchment).
    @cvar OUTLET_INDEX: Outlet object index.
    @cvar INFILTRATION_MODEL: Infiltration model type.
    @cvar FRACTION_IMPERVIOUS: Fraction of impervious area.
    @cvar SUB_AREA_ROUTE_TO: Subarea routing destination.
    @cvar SUB_AREA_FRACTION_OUTLET: Fraction of subarea routed to outlet.
    @cvar SUB_AREA_MANNINGS_N: Subarea Manning's n (sub_index: 0=imperv, 1=perv).
    @cvar SUB_AREA_FRACTION_AREA: Subarea fraction of total area (sub_index: 0=imperv, 1=perv).
    @cvar SUB_AREA_DEPRESSION_STORAGE: Subarea depression storage (sub_index: 0=imperv, 1=perv).
    @cvar SUB_AREA_INFLOW: Subarea inflow (sub_index: 0=imperv, 1=perv).
    @cvar SUB_AREA_RUNOFF: Subarea runoff (sub_index: 0=imperv, 1=perv).
    @cvar SUB_AREA_DEPTH: Subarea depth (sub_index: 0=imperv, 1=perv).
    @cvar LID_UNITS_COUNT: Number of LID units in subcatchment.
    @cvar LID_UNITS_PERV_AREA: LID pervious area.
    @cvar LID_UNITS_FLOW_TO_PERV_AREA: LID flow to pervious area.
    @cvar LID_UNITS_DRAIN_FLOW: LID drain flow.
    @cvar LID_UNIT_REPLICATES: Number of LID unit replicates (sub_index=LID unit index).
    @cvar LID_UNIT_AREA: LID unit area (sub_index=LID unit index).
    @cvar LID_UNIT_FULL_WIDTH: LID unit full top width (sub_index=LID unit index).
    @cvar LID_UNIT_BOTTOM_WIDTH: LID unit bottom width (sub_index=LID unit index).
    @cvar LID_UNIT_INIT_SATURATION: LID unit initial saturation (sub_index=LID unit index).
    @cvar LID_UNIT_FROM_IMPERVIOUS: LID unit fraction from impervious (sub_index=LID unit index).
    @cvar LID_UNIT_FROM_PERVIOUS: LID unit fraction from pervious (sub_index=LID unit index).
    @cvar LID_UNIT_TO_PERVIOUS: LID unit flow sent to pervious (sub_index=LID unit index).
    @cvar LID_UNIT_RECEIVING_OUTLET_TYPE: LID unit drain outlet object type (sub_index=LID unit index).
    @cvar LID_UNIT_RECEIVING_OUTLET_INDEX: LID unit drain outlet index (sub_index=LID unit index).
    @cvar LID_UNIT_SURFACE_DEPTH: LID unit surface depth (sub_index=LID unit index).
    @cvar LID_UNIT_SOIL_MOISTURE: LID unit soil moisture content (sub_index=LID unit index).
    @cvar LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION: LID unit Green-Ampt capillary suction (sub_index=LID unit index).
    @cvar LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY: LID unit Green-Ampt saturated conductivity (sub_index=LID unit index).
    @cvar LID_UNIT_GREEN_AMPT_MAX_SOIL_MOISTURE_DEFICIT: LID unit Green-Ampt max soil moisture deficit (sub_index=LID unit index).
    @cvar CURB_LENGTH: Curb length.
    @cvar API_RAINFALL: API rainfall override.
    @cvar API_SNOWFALL: API snowfall override.
    @cvar POLLUTANT_BUILDUP: Pollutant buildup.
    @cvar EXTERNAL_POLLUTANT_BUILDUP: External pollutant buildup.
    @cvar POLLUTANT_RUNOFF_CONCENTRATION: Pollutant runoff concentration.
    @cvar POLLUTANT_PONDED_CONCENTRATION: Pollutant ponded concentration.
    @cvar POLLUTANT_TOTAL_LOAD: Pollutant total load.
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
    OUTLET_TYPE = swmm_SubcatchProperty.swmm_SUBCATCH_OUTLET_TYPE
    OUTLET_INDEX = swmm_SubcatchProperty.swmm_SUBCATCH_OUTLET_INDEX
    INFILTRATION_MODEL = swmm_SubcatchProperty.swmm_SUBCATCH_INFILTRATION_MODEL
    FRACTION_IMPERVIOUS = swmm_SubcatchProperty.swmm_SUBCATCH_FRACTION_IMPERVIOUS
    SUB_AREA_ROUTE_TO = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_ROUTE_TO
    SUB_AREA_FRACTION_OUTLET = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_FRACTION_OUTLET
    SUB_AREA_MANNINGS_N = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_MANNINGS_N
    SUB_AREA_FRACTION_AREA = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_FRACTION_AREA
    SUB_AREA_DEPRESSION_STORAGE = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_DEPRESSION_STORAGE
    SUB_AREA_INFLOW = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_INFLOW
    SUB_AREA_RUNOFF = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_RUNOFF
    SUB_AREA_DEPTH = swmm_SubcatchProperty.swmm_SUBCATCH_SUB_AREA_DEPTH
    LID_UNITS_COUNT = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNITS_COUNT
    LID_UNITS_PERV_AREA = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNITS_PERV_AREA
    LID_UNITS_FLOW_TO_PERV_AREA = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNITS_FLOW_TO_PERV_AREA
    LID_UNITS_DRAIN_FLOW = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNITS_DRAIN_FLOW
    LID_UNIT_REPLICATES = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_REPLICATES
    LID_UNIT_AREA = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_AREA
    LID_UNIT_FULL_WIDTH = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_FULL_WIDTH
    LID_UNIT_BOTTOM_WIDTH = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_BOTTOM_WIDTH
    LID_UNIT_INIT_SATURATION = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_INIT_SATURATION
    LID_UNIT_FROM_IMPERVIOUS = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_FROM_IMPERVIOUS
    LID_UNIT_FROM_PERVIOUS = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_FROM_PERVIOUS
    LID_UNIT_TO_PERVIOUS = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_TO_PERVIOUS
    LID_UNIT_RECEIVING_OUTLET_TYPE = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_TYPE
    LID_UNIT_RECEIVING_OUTLET_INDEX = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_INDEX
    LID_UNIT_SURFACE_DEPTH = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_SURFACE_DEPTH
    LID_UNIT_SOIL_MOISTURE = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_SOIL_MOISTURE
    LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION
    LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY
    LID_UNIT_GREEN_AMPT_MAX_SOIL_MOISTURE_DEFICIT = swmm_SubcatchProperty.swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_MAXIMUM_SOIL_MOISTURE_DEFICIT
    CURB_LENGTH = swmm_SubcatchProperty.swmm_SUBCATCH_CURB_LENGTH
    API_RAINFALL = swmm_SubcatchProperty.swmm_SUBCATCH_API_RAINFALL
    API_SNOWFALL = swmm_SubcatchProperty.swmm_SUBCATCH_API_SNOWFALL
    POLLUTANT_BUILDUP = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_BUILDUP
    EXTERNAL_POLLUTANT_BUILDUP = swmm_SubcatchProperty.swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP
    POLLUTANT_RUNOFF_CONCENTRATION = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION
    POLLUTANT_PONDED_CONCENTRATION = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION
    POLLUTANT_TOTAL_LOAD = swmm_SubcatchProperty.swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD
    
class SWMMNodeProperties(Enum):
    """Enumeration of SWMM node properties.

    @cvar TYPE: Node type.
    @cvar INVERT_ELEVATION: Invert elevation.
    @cvar MAX_DEPTH: Maximum depth.
    @cvar DEPTH: Depth.
    @cvar HYDRAULIC_HEAD: Hydraulic head.
    @cvar VOLUME: Volume.
    @cvar LATERAL_INFLOW: Lateral inflow.
    @cvar TOTAL_INFLOW: Total inflow.
    @cvar FLOODING: Flooding.
    @cvar REPORT_FLAG: Report flag.
    @cvar SURCHARGE_DEPTH: Surcharge depth.
    @cvar PONDING_AREA: Ponding area.
    @cvar INITIAL_DEPTH: Initial depth.
    @cvar POLLUTANT_CONCENTRATION: Pollutant concentration.
    @cvar POLLUTANT_LATERAL_MASS_FLUX: Pollutant lateral mass flux.
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
    """Enumeration of SWMM link properties.

    @cvar TYPE: Link type.
    @cvar START_NODE: Start node.
    @cvar END_NODE: End node.
    @cvar LENGTH: Length.
    @cvar SLOPE: Slope.
    @cvar FULL_DEPTH: Full depth.
    @cvar FULL_FLOW: Full flow.
    @cvar SETTING: Setting.
    @cvar TIME_OPEN: Time open.
    @cvar TIME_CLOSED: Time closed.
    @cvar FLOW: Flow.
    @cvar DEPTH: Depth.
    @cvar VELOCITY: Velocity.
    @cvar TOP_WIDTH: Top width.
    @cvar VOLUME: Volume.
    @cvar CAPACITY: Capacity.
    @cvar REPORT_FLAG: Report flag.
    @cvar START_NODE_OFFSET: Start node offset.
    @cvar END_NODE_OFFSET: End node offset.
    @cvar INITIAL_FLOW: Initial flow.
    @cvar FLOW_LIMIT: Flow limit.
    @cvar INLET_LOSS: Inlet loss.
    @cvar OUTLET_LOSS: Outlet loss.
    @cvar AVERAGE_LOSS: Average loss.
    @cvar SEEPAGE_RATE: Seepage rate.
    @cvar HAS_FLAPGATE: Has flapgate.
    @cvar POLLUTANT_CONCENTRATION: Pollutant concentration.
    @cvar POLLUTANT_LOAD: Pollutant load.
    @cvar POLLUTANT_LATERAL_MASS_FLUX: Pollutant lateral mass flux.
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
    """Enumeration of SWMM link types.

    @cvar CONDUIT: Conduit link.
    @cvar PUMP: Pump link.
    @cvar ORIFICE: Orifice link.
    @cvar WEIR: Weir link.
    @cvar OUTLET: Outlet link.
    """
    CONDUIT = swmm_LinkType.swmm_CONDUIT
    PUMP = swmm_LinkType.swmm_PUMP
    ORIFICE = swmm_LinkType.swmm_ORIFICE
    WEIR = swmm_LinkType.swmm_WEIR
    OUTLET = swmm_LinkType.swmm_OUTLET

class SWMMSystemProperties(Enum):
    """Enumeration of SWMM system properties.

    @cvar START_DATE: Start date for the simulation.
    @cvar CURRENT_DATE: Current date for the simulation.
    @cvar ELAPSED_TIME: Elapsed time for the simulation.
    @cvar ROUTING_STEP: Routing time step.
    @cvar MAX_ROUTING_STEP: Maximum routing time step.
    @cvar REPORT_STEP: Report time step.
    @cvar TOTAL_STEPS: Total number of steps.
    @cvar NO_REPORT_FLAG: No report flag.
    @cvar FLOW_UNITS: Flow units.
    @cvar END_DATE: End date for the simulation.
    @cvar REPORT_START_DATE: Report start date.
    @cvar UNIT_SYSTEM: Unit system.
    @cvar SURCHARGE_METHOD: Surcharge method.
    @cvar ALLOW_PONDING: Allow ponding.
    @cvar INTERTIAL_DAMPING: Inertial damping.
    @cvar NORMAL_FLOW_LIMITED: Normal flow limited.
    @cvar SKIP_STEADY_STATE: Skip steady state.
    @cvar IGNORE_RAINFALL: Ignore rainfall.
    @cvar IGNORE_RDII: Ignore RDII.
    @cvar IGNORE_SNOWMELT: Ignore snowmelt.
    @cvar IGNORE_GROUNDWATER: Ignore groundwater.
    @cvar IGNORE_ROUTING: Ignore routing.
    @cvar IGNORE_QUALITY: Ignore quality.
    @cvar ERROR_CODE: Most recent internal error code.
    @cvar RULE_STEP: Rule step.
    @cvar SWEEP_START: Sweep start.
    @cvar SWEEP_END: Sweep end.
    @cvar MAX_TRIALS: Maximum trials.
    @cvar NUM_THREADS: Number of threads.
    @cvar MIN_ROUTE_STEP: Minimum routing step.
    @cvar LENGTHENING_STEP: Lengthening step.
    @cvar START_DRY_DAYS: Start dry days.
    @cvar COURANT_FACTOR: Courant factor.
    @cvar MIN_SURF_AREA: Minimum surface area.
    @cvar MIN_SLOPE: Minimum slope.
    @cvar RUNOFF_ERROR: Runoff error.
    @cvar FLOW_ERROR: Flow error.
    @cvar QUAL_ERROR: Quality error.
    @cvar HEAD_TOL: Head tolerance.
    @cvar SYS_FLOW_TOL: System flow tolerance.
    @cvar LAT_FLOW_TOL: Lateral flow tolerance.
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

# =============================================================================
# Units / errors enumerations
# =============================================================================

class SWMMFlowUnits(Enum):
    """Enumeration of SWMM flow units.

    @cvar CFS: Cubic feet per second.
    @cvar GPM: Gallons per minute.
    @cvar MGD: Million gallons per day.
    @cvar CMS: Cubic meters per second.
    @cvar LPS: Liters per second.
    @cvar MLD: Million liters per day.
    """
    CFS = swmm_FlowUnitsProperty.swmm_CFS
    GPM = swmm_FlowUnitsProperty.swmm_GPM
    MGD = swmm_FlowUnitsProperty.swmm_MGD
    CMS = swmm_FlowUnitsProperty.swmm_CMS
    LPS = swmm_FlowUnitsProperty.swmm_LPS
    MLD = swmm_FlowUnitsProperty.swmm_MLD

class SWMMAPIErrors(Enum):
    """Enumeration of SWMM API errors.

    @cvar PROJECT_NOT_OPENED: Project not opened.
    @cvar SIMULATION_NOT_STARTED: Simulation not started.
    @cvar SIMULATION_NOT_ENDED: Simulation not ended.
    @cvar OBJECT_TYPE: Invalid object type.
    @cvar OBJECT_INDEX: Invalid object index.
    @cvar OBJECT_NAME: Invalid object name.
    @cvar PROPERTY_TYPE: Invalid property type.
    @cvar PROPERTY_VALUE: Invalid property value.
    @cvar TIME_PERIOD: Invalid time period.
    @cvar HOTSTART_FILE_OPEN: Error opening hotstart file.
    @cvar HOTSTART_FILE_FORMAT: Invalid hotstart file format.
    @cvar SIMULATION_IS_RUNNING: Simulation is running.
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

# =============================================================================
# Internal callback wrappers
# =============================================================================

cdef void c_wrapper_function(double x):
    """Wrapper function to call a Python function from C.

    @param x: Input value forwarded to the registered Python callback.
    @type x: float
    """
    global py_progress_callback
    cdef tuple args = (x,)
    PyObject_CallObject(py_progress_callback, args)

cdef progress_callback wrap_python_function_as_callback(object py_func):
    """Wrap a Python function as a SWMM C C{progress_callback}.

    @param py_func: Python function to invoke for each progress update.
    @type py_func: callable
    @return: C-callable progress callback that forwards to C{py_func}.
    @rtype: progress_callback
    """
    global py_progress_callback
    py_progress_callback = py_func
    return <progress_callback>c_wrapper_function

cdef object global_solver = None

cdef void progress_callback_wrapper(double progress):
    """Wrapper function dispatching the C progress callback to a solver instance method.

    @param progress: Progress fraction in C{[0.0, 1.0]}.
    @type progress: float
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
    """Run a SWMM simulation, optionally with a progress callback.

    @param inp_file: Input file name.
    @type inp_file: str
    @param rpt_file: Report file name.
    @type rpt_file: str
    @param out_file: Output file name.
    @type out_file: str
    @param swmm_progress_callback: Optional progress callback receiving a
        float between 0.0 and 1.0.
    @type swmm_progress_callback: callable
    @return: Error code from the underlying SWMM C API (C{0} on success,
        non-zero on failure). Callers should check the return value rather
        than relying on an exception.
    @rtype: int
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

    return error_code

cpdef cython_datetime decode_swmm_datetime(double swmm_datetime):
    """Decode a SWMM datetime float into a L{datetime} object.

    @param swmm_datetime: SWMM datetime float value.
    @type swmm_datetime: float
    @return: Decoded datetime.
    @rtype: datetime
    """
    cdef int year, month, day, hour, minute, second, day_of_week
    swmm_decodeDate(swmm_datetime, &year, &month, &day, &hour, &minute, &second, &day_of_week)

    return datetime(year, month, day, hour, minute, second)

cpdef double encode_swmm_datetime(cython_datetime dt):
    """Encode a L{datetime} object into a SWMM datetime float value.

    @param dt: datetime object.
    @type dt: datetime
    @return: SWMM datetime float value.
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
    """Get the SWMM version.

    @return: SWMM version (encoded as integer).
    @rtype: int
    """
    cdef int swmm_version = swmm_getVersion()

    return swmm_version

cpdef str get_error_message(int error_code):
    """Get the error message text for a SWMM error code.

    @param error_code: Error code.
    @type error_code: int
    @return: Error message.
    @rtype: str
    """
    cdef char* c_error_message = <char*>malloc(1024*sizeof(char))
    
    swmm_getErrorFromCode(error_code, &c_error_message)

    error_message = c_error_message.decode('utf-8')

    free(c_error_message)

    return error_message

# =============================================================================
# Lifecycle / callback enumerations and exceptions
# =============================================================================

class SolverState(Enum):
    """Enumeration to represent the state of the solver.

    @cvar CREATED: Solver instance created but not yet opened.
    @cvar OPEN: Input file opened.
    @cvar STARTED: Simulation started.
    @cvar FINISHED: Stepping has completed.
    @cvar ENDED: Simulation ended.
    @cvar REPORTED: Results reported.
    @cvar CLOSED: Solver closed.
    """
    CREATED = 0
    OPEN = 1
    STARTED = 2
    FINISHED = 3
    ENDED = 4
    REPORTED = 5
    CLOSED = 6

class CallbackType(Enum):
    """Enumeration of callback hook points exposed by L{Solver.add_callback}.

    @cvar BEFORE_INITIALIZE: Fired before C{initialize}.
    @cvar BEFORE_OPEN: Fired before C{open}.
    @cvar AFTER_OPEN: Fired after C{open}.
    @cvar BEFORE_START: Fired before C{start}.
    @cvar AFTER_START: Fired after C{start}.
    @cvar BEFORE_STEP: Fired before each C{step}.
    @cvar AFTER_STEP: Fired after each C{step}.
    @cvar BEFORE_END: Fired before C{end}.
    @cvar AFTER_END: Fired after C{end}.
    @cvar BEFORE_REPORT: Fired before C{report}.
    @cvar AFTER_REPORT: Fired after C{report}.
    @cvar BEFORE_CLOSE: Fired before C{close}.
    @cvar AFTER_CLOSE: Fired after C{close}.
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
    """Exception class for SWMM solver errors."""
    def __init__(self, message: str) -> None:
        """Constructor to initialize the exception message.

        @param message: Error message.
        @type message: str
        """
        super().__init__(message)

cdef class Solver:
    """A class to represent a SWMM solver.

    The solver wraps the legacy SWMM 5.x C API lifecycle
    (open / start / step / end / report / close) and adds Python
    conveniences such as context-manager support, iterator-style
    stepping, and callback hooks.
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
    # Lifecycle
    # ====================================================================

    def __cinit__(
        self,
        str inp_file,
        str rpt_file = None,
        str out_file = None,
        int stride_step = 300,
        bint save_results=True
    ):
        """Constructor to create a new SWMM solver.

        @param inp_file: Input file name.
        @type inp_file: str
        @param rpt_file: Report file name. Derived from C{inp_file} when omitted.
        @type rpt_file: str
        @param out_file: Output file name. Derived from C{inp_file} when omitted.
        @type out_file: str
        @param stride_step: Default stride (in seconds) used by C{step()}.
        @type stride_step: int
        @param save_results: Whether to save binary results.
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
        """Enter the runtime context, opening the solver.

        @return: This solver instance.
        @rtype: L{Solver}
        """
        self.__execute_callbacks(CallbackType.BEFORE_INITIALIZE)
        self.open()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        """Exit the runtime context, finalizing the solver."""
        self.finalize()

    def __dealloc__(self):
        """Destructor to free the solver."""
        self.finalize()

    def __iter__(self):
        """Return the solver as an iterator that yields step results.

        @return: This solver instance.
        @rtype: L{Solver}
        """
        return self

    def __next__(self) -> int:
        """Advance one step.

        Returns the per-step error code from L{step}. Iterate, checking the
        return code to detect failure; iteration stops automatically when
        the solver transitions to C{FINISHED}::

            for rc in solver:
                if rc != 0:
                    break

        @return: Error code (C{0} on success, non-zero on failure).
        @rtype: int
        @raise StopIteration: When the solver has finished.
        """
        if self._solver_state == SolverState.FINISHED:
            raise StopIteration
        return self.step()

    # ====================================================================
    # Timing properties
    # ====================================================================

    @property
    def start_datetime(self) -> datetime:
        """Get the start date of the simulation.

        @return: Start date.
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
        """Set the start date of the simulation.

        @param sim_start_datetime: Start date of the simulation.
        @type sim_start_datetime: datetime
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
        """Get the end date of the simulation.

        @return: End date.
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
        """Set the end date of the simulation.

        @param sim_end_datetime: End date of the simulation.
        @type sim_end_datetime: datetime
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
        """Get the routing time step of the simulation in seconds.

        @return: Routing time step.
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
        """Set the routing time step of the simulation in seconds.

        @param value: Routing time step in seconds.
        @type value: float
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
        """Get the reporting time step of the simulation in seconds.

        @return: Reporting time step.
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
        """Set the reporting time step of the simulation in seconds.

        @param value: Reporting time step in seconds.
        @type value: float
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
        """Get the report start date of the simulation.

        @return: Report start date.
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
        """Set the report start date of the simulation.

        @param report_start_datetime: Report start date.
        @type report_start_datetime: datetime
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
        """Get the current date of the simulation.

        @return: Current date.
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
    def progress_callbacks_per_second(self) -> int:
        """Get the maximum number of progress callbacks per second.

        @return: Progress callbacks per second.
        @rtype: int
        """
        return self._progress_callbacks_per_second

    @progress_callbacks_per_second.setter
    def progress_callbacks_per_second(self, value: int) -> None:
        """Set the maximum number of progress callbacks per second.

        @param value: Progress callbacks per second.
        @type value: int
        """
        self._progress_callbacks_per_second = max(1, value)

    # ====================================================================
    # Per-element queries
    # ====================================================================

    def get_object_count(self, object_type: SWMMObjects) -> int:
        """Get the count of a SWMM object type.

        @param object_type: SWMM object type.
        @type object_type: L{SWMMObjects}
        @return: Object count.
        @rtype: int
        """
        cdef int count = swmm_getCount(objType=object_type.value)

        self.__validate_error(count)

        return count
    
    def get_object_name(self, object_type: SWMMObjects, index: int) -> str:
        """Get the name of a SWMM object.

        @param object_type: SWMM object type.
        @type object_type: L{SWMMObjects}
        @param index: Object index.
        @type index: int
        @return: Object name.
        @rtype: str
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
        """Get the names of all SWMM objects of a given type.

        @param object_type: SWMM object type.
        @type object_type: L{SWMMObjects}
        @return: Object names.
        @rtype: List[str]
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
        """Get the index of a SWMM object by name.

        @param object_type: SWMM object type.
        @type object_type: L{SWMMObjects}
        @param object_name: Object name.
        @type object_name: str
        @return: Object index.
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
        """Set a SWMM property value.

        @param object_type: SWMM object type (e.g., C{SWMMObjects.NODE}).
        @type object_type: L{SWMMObjects}
        @param property_type: SWMM property type (e.g., C{SWMMSystemProperties.START_DATE}).
        @type property_type: Union[SWMMRainGageProperties, SWMMSubcatchmentProperties, SWMMNodeProperties, SWMMLinkProperties, SWMMSystemProperties]
        @param index: Object index or name (e.g., C{0} or C{"J1"}).
        @type index: int or str
        @param value: Property value (e.g., C{10.0}).
        @type value: float
        @param sub_index: Sub-index (e.g., C{0}) for properties with sub-indexes.
            For example, the pollutant index for POLLUTANT properties.
        @type sub_index: int
        @param pollutant_index: Pollutant index (e.g., C{0}) for POLLUTANT properties.
        @type pollutant_index: int
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
        """Get a SWMM property value.

        @param object_type: SWMM object type (e.g., C{SWMMObjects.NODE}).
        @type object_type: L{SWMMObjects}
        @param property_type: SWMM property type (e.g., C{SWMMSystemProperties.START_DATE}).
        @type property_type: Union[SWMMRainGageProperties, SWMMSubcatchmentProperties, SWMMNodeProperties, SWMMLinkProperties, SWMMSystemProperties]
        @param index: Object index or name (e.g., C{0} or C{"J1"}).
        @type index: int or str
        @param sub_index: Sub-index (e.g., C{0}) for properties with sub-indexes.
            For example, the pollutant index for POLLUTANT properties.
        @type sub_index: int
        @param pollutant_index: Pollutant index (e.g., C{0}) for POLLUTANT properties.
        @type pollutant_index: int
        @return: Property value.
        @rtype: float
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
    
    @property
    def stride_step(self) -> int:
        """Get the stride step of the simulation.

        @return: Stride step in seconds.
        @rtype: int
        """
        return self._stride_step

    @stride_step.setter
    def stride_step(self, value: int):
        """Set the stride time step of the simulation.

        @param value: Stride step in seconds.
        @type value: int
        """
        self._stride_step = value

    @property
    def solver_state(self) -> SolverState:
        """Get the state of the solver.

        @return: Solver state.
        @rtype: L{SolverState}
        """
        return self._solver_state

    # ====================================================================
    # Callbacks
    # ====================================================================

    def add_callback(self, callback_type: CallbackType, callback: Callable[[Solver], None]) -> None:
        """Add a callback to the solver.

        @param callback_type: Type of callback.
        @type callback_type: L{CallbackType}
        @param callback: Callback function (receives the solver instance).
        @type callback: callable
        """
        self._callbacks[callback_type].append(callback)

    def add_progress_callback(self, callback: Callable[[float], None]) -> None:
        """Add a progress callback to the solver.

        @param callback: Progress callback function receiving a float
            between 0.0 and 1.0.
        @type callback: callable
        """
        self._progress_callbacks.append(callback)

    # ====================================================================
    # Lifecycle methods
    # ====================================================================

    cpdef int open(self):
        """Open the SWMM solver by calling the open method in the SWMM API.

        @return: Error code (C{0} on success or no-op, non-zero on failure).
            Returns C{-1} if the solver is in a state from which C{open} is
            not valid; lifecycle status is queryable via L{solver_state}.
        @rtype: int
        """
        cdef int error_code = 0
        cdef bytes c_inp_file_bytes = self._inp_file.encode('utf-8')
        cdef bytes c_rpt_file_bytes = self._rpt_file.encode('utf-8')
        cdef bytes c_out_file_bytes = self._out_file.encode('utf-8')

        cdef const char* c_inp_file = c_inp_file_bytes
        cdef const char* c_rpt_file = c_rpt_file_bytes
        cdef const char* c_out_file = c_out_file_bytes

        if self._solver_state == SolverState.OPEN:
            return 0
        if self._solver_state != SolverState.CREATED and \
           self._solver_state != SolverState.CLOSED:
            return -1

        self.__execute_callbacks(CallbackType.BEFORE_OPEN)

        error_code = swmm_open(
            inp_file=c_inp_file,
            rpt_file=c_rpt_file,
            out_file=c_out_file
        )
        if error_code != 0:
            return error_code

        self._solver_state = SolverState.OPEN
        self.__execute_callbacks(CallbackType.AFTER_OPEN)
        self._clock = clock()

        self._total_duration = swmm_getValue(
            property=SWMMSystemProperties.END_DATE.value,
            index=0
        ) - swmm_getValue(
            property=SWMMSystemProperties.START_DATE.value,
            index=0
        )
        return 0

    cpdef int start(self):
        """Start the SWMM solver.

        @return: Error code (C{0} on success or no-op, non-zero on failure).
            Returns C{-1} if not in C{OPEN} state.
        @rtype: int
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.STARTED:
            return 0
        if self._solver_state != SolverState.OPEN:
            return -1

        self.__execute_callbacks(CallbackType.BEFORE_START)
        error_code = swmm_start(save_flag=self._save_results)
        if error_code != 0:
            return error_code
        self._solver_state = SolverState.STARTED
        self.__execute_callbacks(CallbackType.AFTER_START)
        return 0

    cpdef int initialize(self):
        """Initialize the solver and start the simulation.

        Calls the open and start methods in the SWMM API in sequence.

        @return: Error code (C{0} on success, non-zero from the first
            failing sub-call).
        @rtype: int
        """
        cdef int rc
        self.__execute_callbacks(CallbackType.BEFORE_INITIALIZE)
        rc = self.open()
        if rc != 0:
            return rc
        return self.start()

    cpdef int step(self, int steps = 0):
        """Step a SWMM simulation forward.

        Caller polls L{solver_state} (transitions to C{FINISHED} when the
        simulation is complete) and reads L{current_datetime}/L{elapsed} for
        per-step data::

            from openswmm.legacy.engine import SolverState
            while solver.solver_state == SolverState.STARTED:
                rc = solver.step()
                if rc != 0:
                    break

        @param steps: Number of seconds to advance. Overrides the internal
            stride step when greater than 0.
        @type steps: int
        @return: Error code from the underlying SWMM C API (C{0} on success,
            non-zero on failure).
        @rtype: int
        """
        cdef double elapsed_time = 0.0
        cdef double progress = 0.0
        cdef int error_code

        error_code = swmm_stride(strideStep=steps if steps > 0 else self._stride_step, elapsedTime=&elapsed_time)
        if error_code != 0:
            return error_code

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

        return 0

    cpdef int end(self):
        """End the SWMM simulation.

        @return: Error code (C{0} on success or no-op, non-zero on failure).
            Returns C{-1} if the solver is in a state from which C{end} is
            not valid.
        @rtype: int
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.ENDED or \
           self._solver_state == SolverState.CREATED:
            return 0
        if self._solver_state != SolverState.OPEN and \
           self._solver_state != SolverState.STARTED and \
           self._solver_state != SolverState.FINISHED:
            return -1

        self.__execute_callbacks(CallbackType.BEFORE_END)
        error_code = swmm_end()
        if error_code != 0:
            return error_code
        self._solver_state = SolverState.ENDED
        self.__execute_callbacks(CallbackType.AFTER_END)
        return 0

    cpdef int report(self):
        """Write the SWMM report file.

        @return: Error code (C{0} on success or no-op, non-zero on failure).
            Returns C{-1} if the solver is not in C{ENDED} state.
        @rtype: int
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.REPORTED or self._solver_state == SolverState.CREATED:
            return 0
        if self._solver_state != SolverState.ENDED:
            return -1

        self.__execute_callbacks(CallbackType.BEFORE_REPORT)
        error_code = swmm_report()
        if error_code != 0:
            return error_code
        self._solver_state = SolverState.REPORTED
        self.__execute_callbacks(CallbackType.AFTER_REPORT)
        return 0

    cpdef int close(self):
        """Close the solver.

        @return: Error code (C{0} on success or no-op, non-zero on failure).
            Returns C{-1} if the solver is not in C{REPORTED} state.
        @rtype: int
        """
        cdef int error_code = 0

        if self._solver_state == SolverState.CREATED:
            return 0
        if self._solver_state != SolverState.REPORTED:
            return -1

        self.__execute_callbacks(CallbackType.BEFORE_CLOSE)
        error_code = swmm_close()
        if error_code != 0:
            return error_code
        self._solver_state = SolverState.CLOSED
        self.__execute_callbacks(CallbackType.AFTER_CLOSE)
        return 0

    cpdef void finalize(self):
        """Finalize the solver.

        Ends the simulation, reports results and disposes of objects, calling
        each lifecycle method only if it has not already been invoked.
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
        """Run the solver to completion.

        @raise SWMMSolverException: If the solver is in an invalid state.
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
    # Hot start I/O
    # ====================================================================

    cpdef void use_hotstart(self, str hotstart_file):
        """Use a hotstart file as input.

        @param hotstart_file: Hotstart file name.
        @type hotstart_file: str
        """
        cdef bytes c_hotstart_file = hotstart_file.encode('utf-8')
        cdef const char* cc_hotstart_file = c_hotstart_file
        cdef int error_code = swmm_useHotStart(hotStartFile=cc_hotstart_file)

        self.__validate_error(error_code)
    
    cpdef void save_hotstart(self, str hotstart_file):
        """Save the current state to a hotstart file.

        @param hotstart_file: Hotstart file name.
        @type hotstart_file: str
        """
        cdef bytes c_hotstart_file = hotstart_file.encode('utf-8')
        cdef const char* cc_hotstart_file = c_hotstart_file
        cdef int error_code = swmm_saveHotStart(hotStartFile=cc_hotstart_file)

        self.__validate_error(error_code)

    # ====================================================================
    # Diagnostics
    # ====================================================================

    def get_mass_balance_error(self) -> Tuple[float, float, float]:
        """Get the mass balance error percentages after simulation ends.

        @return: Tuple of C{(runoff_error%, flow_error%, quality_error%)}.
        @rtype: Tuple[float, float, float]
        """
        cdef float runoffErr, flowErr, qualErr

        cdef int error_code = swmm_getMassBalErr(
            runoffErr=&runoffErr,
            flowErr=&flowErr,
            qualErr=&qualErr
        )

        self.__validate_error(error_code)

        return (runoffErr, flowErr, qualErr)

    def get_warnings(self) -> int:
        """Get the number of warnings issued during the simulation.

        @return: Number of warning messages.
        @rtype: int
        """
        return swmm_getWarnings()

    def write_line(self, text: str) -> None:
        """Write a line of text to the SWMM report file.

        @param text: Line of text to write.
        @type text: str
        """
        cdef bytes c_text = text.encode('utf-8')
        cdef const char* c_line = c_text
        swmm_writeLine(c_line)

    def get_saved_value(
        self,
        property_type: Union[
            SWMMSubcatchmentProperties,
            SWMMNodeProperties,
            SWMMLinkProperties,
            SWMMSystemProperties
        ],
        index: Union[int, str],
        period: int
    ) -> float:
        """Get a saved value from the binary output file during simulation.

        Reads a previously saved result value from the output file for the
        specified property, object index, and reporting period.

        @param property_type: Property to retrieve.
        @type property_type: Union[SWMMSubcatchmentProperties, SWMMNodeProperties, SWMMLinkProperties, SWMMSystemProperties]
        @param index: Object index or name.
        @type index: int or str
        @param period: Reporting period index (0-based).
        @type period: int
        @return: Saved property value.
        @rtype: float
        """
        cdef int element_index = -1

        if isinstance(index, str):
            # Determine object type from property type for name resolution
            if isinstance(property_type, SWMMSubcatchmentProperties):
                element_index = self.get_object_index(SWMMObjects.SUBCATCHMENT, index)
            elif isinstance(property_type, SWMMNodeProperties):
                element_index = self.get_object_index(SWMMObjects.NODE, index)
            elif isinstance(property_type, SWMMLinkProperties):
                element_index = self.get_object_index(SWMMObjects.LINK, index)
            else:
                element_index = 0
            self.__validate_error(element_index)
        else:
            element_index = index

        cdef double value = swmm_getSavedValue(
            property=<int>property_type.value,
            index=element_index,
            period=period
        )

        return value

    # ====================================================================
    # Statistics and mass balance (Phase 2 -- call after end() / before close())
    # ====================================================================

    def get_subcatchment_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative subcatchment statistics.

        Must be called after L{end} and before L{close}.

        @param index: Subcatchment index or name.
        @type index: int or str
        @return: Dictionary with keys: C{precip}, C{runon}, C{evap},
            C{infil}, C{runoff}, C{max_flow}, C{imperv_runoff},
            C{perv_runoff}.
        @rtype: dict
        """
        cdef swmm_SubcatchStats stats
        cdef int idx = self.__resolve_index(SWMMObjects.SUBCATCHMENT, index)
        cdef int err = swmm_getSubcatchStats(idx, &stats)
        self.__validate_error(err)
        return {
            'precip': stats.precip,
            'runon': stats.runon,
            'evap': stats.evap,
            'infil': stats.infil,
            'runoff': stats.runoff,
            'max_flow': stats.maxFlow,
            'imperv_runoff': stats.impervRunoff,
            'perv_runoff': stats.pervRunoff,
        }

    def get_node_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative node statistics.

        Must be called after L{end} and before L{close}.

        @param index: Node index or name.
        @type index: int or str
        @return: Dictionary with keys: C{avg_depth}, C{max_depth},
            C{max_depth_date}, C{max_rpt_depth}, C{vol_flooded},
            C{time_flooded}, C{time_surcharged}, C{time_courant_critical},
            C{tot_lat_flow}, C{max_lat_flow}, C{max_inflow},
            C{max_overflow}, C{max_ponded_vol}, C{max_inflow_date},
            C{max_overflow_date}.
        @rtype: dict
        """
        cdef swmm_NodeStats stats
        cdef int idx = self.__resolve_index(SWMMObjects.NODE, index)
        cdef int err = swmm_getNodeStats(idx, &stats)
        self.__validate_error(err)
        return {
            'avg_depth': stats.avgDepth,
            'max_depth': stats.maxDepth,
            'max_depth_date': decode_swmm_datetime(stats.maxDepthDate),
            'max_rpt_depth': stats.maxRptDepth,
            'vol_flooded': stats.volFlooded,
            'time_flooded': stats.timeFlooded,
            'time_surcharged': stats.timeSurcharged,
            'time_courant_critical': stats.timeCourantCritical,
            'tot_lat_flow': stats.totLatFlow,
            'max_lat_flow': stats.maxLatFlow,
            'max_inflow': stats.maxInflow,
            'max_overflow': stats.maxOverflow,
            'max_ponded_vol': stats.maxPondedVol,
            'max_inflow_date': decode_swmm_datetime(stats.maxInflowDate),
            'max_overflow_date': decode_swmm_datetime(stats.maxOverflowDate),
        }

    def get_storage_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative storage node statistics.

        @param index: Node index or name (must be a STORAGE node).
        @type index: int or str
        @return: Dictionary with keys: C{init_vol}, C{avg_vol}, C{max_vol},
            C{max_flow}, C{evap_losses}, C{exfil_losses}, C{max_vol_date}.
        @rtype: dict
        """
        cdef swmm_StorageStats stats
        cdef int idx = self.__resolve_index(SWMMObjects.NODE, index)
        cdef int err = swmm_getStorageStats(idx, &stats)
        self.__validate_error(err)
        return {
            'init_vol': stats.initVol,
            'avg_vol': stats.avgVol,
            'max_vol': stats.maxVol,
            'max_flow': stats.maxFlow,
            'evap_losses': stats.evapLosses,
            'exfil_losses': stats.exfilLosses,
            'max_vol_date': decode_swmm_datetime(stats.maxVolDate),
        }

    def get_outfall_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative outfall node statistics.

        @param index: Node index or name (must be an OUTFALL node).
        @type index: int or str
        @return: Dictionary with keys: C{avg_flow}, C{max_flow},
            C{total_periods}, C{total_load} (list of per-pollutant loads).
        @rtype: dict
        """
        cdef int p
        cdef int n_polluts = swmm_getCount(objType=SWMMObjects.POLLUTANT.value)
        cdef swmm_OutfallStats stats
        cdef double *loads = NULL

        if n_polluts > 0:
            loads = <double *>malloc(n_polluts * sizeof(double))
            if loads == NULL:
                raise MemoryError("Cannot allocate pollutant load array")
            stats.totalLoad = loads
        else:
            stats.totalLoad = NULL

        cdef int idx = self.__resolve_index(SWMMObjects.NODE, index)
        cdef int err = swmm_getOutfallStats(idx, &stats)

        if err != 0:
            free(loads)
            self.__validate_error(err)

        result = {
            'avg_flow': stats.avgFlow,
            'max_flow': stats.maxFlow,
            'total_periods': stats.totalPeriods,
            'total_load': [loads[p] for p in range(n_polluts)] if loads != NULL else [],
        }
        free(loads)
        return result

    def get_link_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative link statistics.

        @param index: Link index or name.
        @type index: int or str
        @return: Dictionary with keys: C{max_flow}, C{max_flow_date},
            C{max_veloc}, C{max_depth}, C{time_normal_flow},
            C{time_inlet_control}, C{time_surcharged}, C{time_full_upstream},
            C{time_full_dnstream}, C{time_full_flow},
            C{time_capacity_limited}, C{time_courant_critical},
            C{flow_turns}, C{flow_turn_sign},
            C{time_in_flow_class} (list of 7 floats).
        @rtype: dict
        """
        cdef swmm_LinkStats stats
        cdef int idx = self.__resolve_index(SWMMObjects.LINK, index)
        cdef int err = swmm_getLinkStats(idx, &stats)
        self.__validate_error(err)
        return {
            'max_flow': stats.maxFlow,
            'max_flow_date': decode_swmm_datetime(stats.maxFlowDate),
            'max_veloc': stats.maxVeloc,
            'max_depth': stats.maxDepth,
            'time_normal_flow': stats.timeNormalFlow,
            'time_inlet_control': stats.timeInletControl,
            'time_surcharged': stats.timeSurcharged,
            'time_full_upstream': stats.timeFullUpstream,
            'time_full_dnstream': stats.timeFullDnstream,
            'time_full_flow': stats.timeFullFlow,
            'time_capacity_limited': stats.timeCapacityLimited,
            'time_courant_critical': stats.timeCourantCritical,
            'flow_turns': stats.flowTurns,
            'flow_turn_sign': stats.flowTurnSign,
            'time_in_flow_class': [stats.timeInFlowClass[k] for k in range(7)],
        }

    def get_pump_statistics(self, index: Union[int, str]) -> dict:
        """Get cumulative pump statistics.

        @param index: Link index or name (must be a PUMP link).
        @type index: int or str
        @return: Dictionary with keys: C{utilized}, C{min_flow}, C{avg_flow},
            C{max_flow}, C{volume}, C{energy}, C{off_curve_low},
            C{off_curve_high}, C{start_ups}, C{total_periods}.
        @rtype: dict
        """
        cdef swmm_PumpStats stats
        cdef int idx = self.__resolve_index(SWMMObjects.LINK, index)
        cdef int err = swmm_getPumpStats(idx, &stats)
        self.__validate_error(err)
        return {
            'utilized': stats.utilized,
            'min_flow': stats.minFlow,
            'avg_flow': stats.avgFlow,
            'max_flow': stats.maxFlow,
            'volume': stats.volume,
            'energy': stats.energy,
            'off_curve_low': stats.offCurveLow,
            'off_curve_high': stats.offCurveHigh,
            'start_ups': stats.startUps,
            'total_periods': stats.totalPeriods,
        }

    def get_routing_totals(self) -> dict:
        """Get system-level flow routing mass balance totals.

        All volumes are in project units (ft3 for US, m3 for SI).
        Must be called after L{end} and before L{close}.

        Mass balance equation::

            total_inflow = dw + ww + gw + ii + ex
            total_outflow = flooding + outflow + evap_loss + seep_loss
            storage_change = final_storage - init_storage
            balance = total_inflow - total_outflow - storage_change
            # balance / total_inflow ~= pct_error / 100

        @return: Dictionary with keys: C{dw_inflow}, C{ww_inflow},
            C{gw_inflow}, C{ii_inflow}, C{ex_inflow}, C{flooding},
            C{outflow}, C{evap_loss}, C{seep_loss}, C{reacted},
            C{init_storage}, C{final_storage}, C{pct_error}.
        @rtype: dict
        """
        cdef swmm_RoutingTotals totals
        cdef int err = swmm_getSystemRoutingTotals(&totals)
        self.__validate_error(err)
        return {
            'dw_inflow': totals.dwInflow,
            'ww_inflow': totals.wwInflow,
            'gw_inflow': totals.gwInflow,
            'ii_inflow': totals.iiInflow,
            'ex_inflow': totals.exInflow,
            'flooding': totals.flooding,
            'outflow': totals.outflow,
            'evap_loss': totals.evapLoss,
            'seep_loss': totals.seepLoss,
            'reacted': totals.reacted,
            'init_storage': totals.initStorage,
            'final_storage': totals.finalStorage,
            'pct_error': totals.pctError,
        }

    def get_runoff_totals(self) -> dict:
        """Get system-level surface runoff mass balance totals.

        Must be called after L{end} and before L{close}.

        Mass balance equation::

            rainfall = evap + infil + runoff + storage_change + error
            storage_change = final_storage - init_storage

        @return: Dictionary with keys: C{rainfall}, C{evap}, C{infil},
            C{runoff}, C{drains}, C{runon}, C{init_storage},
            C{final_storage}, C{init_snow_cover}, C{final_snow_cover},
            C{snow_removed}, C{pct_error}.
        @rtype: dict
        """
        cdef swmm_RunoffTotals totals
        cdef int err = swmm_getSystemRunoffTotals(&totals)
        self.__validate_error(err)
        return {
            'rainfall': totals.rainfall,
            'evap': totals.evap,
            'infil': totals.infil,
            'runoff': totals.runoff,
            'drains': totals.drains,
            'runon': totals.runon,
            'init_storage': totals.initStorage,
            'final_storage': totals.finalStorage,
            'init_snow_cover': totals.initSnowCover,
            'final_snow_cover': totals.finalSnowCover,
            'snow_removed': totals.snowRemoved,
            'pct_error': totals.pctError,
        }

    # ====================================================================
    # Private helpers
    # ====================================================================

    cdef int __resolve_index(self, object_type, index):
        """Resolve a name or integer index to an integer index.

        @param object_type: Object type used for name -> index lookup.
        @type object_type: L{SWMMObjects}
        @param index: Object index (int) or name (str).
        @type index: int or str
        @return: Resolved integer index.
        @rtype: int
        """
        cdef int element_index
        if isinstance(index, str):
            element_index = self.get_object_index(object_type, index)
            self.__validate_error(element_index)
            return element_index
        return <int>index

    def __execute_callbacks(self, callback_type: CallbackType) -> None:
        """Execute the callbacks registered for the given type.

        @param callback_type: Type of callback.
        @type callback_type: L{CallbackType}
        """
        for callback in self._callbacks[callback_type]:
            callback(self)

    cpdef void __execute_progress_callbacks(self, double percent_complete):
        """Execute all registered progress callbacks.

        @param percent_complete: Progress fraction in C{[0.0, 1.0]}.
        @type percent_complete: float
        """
        for callback in self._progress_callbacks:
            callback(percent_complete)

    cdef void __progress_callback(self, double percent_complete):
        """Internal progress callback rate-limited by C{progress_callbacks_per_second}.

        @param percent_complete: Progress fraction in C{[0.0, 1.0]}.
        @type percent_complete: float
        """
        cdef clock_t elapsed_time =   clock() - self._clock

        if elapsed_time > 1.0 / self._progress_callbacks_per_second:
            self.__execute_progress_callbacks(
                percent_complete=percent_complete
            )

            self._clock = clock()

    cdef void __validate_error(self, error_code: int) :
        """Validate a C API return code and raise on error.

        @param error_code: Error code to validate.
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
        """Get the most recent solver error message string.

        @return: Error message text.
        @rtype: str
        """
        cdef char* c_error_message = <char*>malloc(1024*sizeof(char))
        swmm_getError(c_error_message, 1024)

        error_message = c_error_message.decode('utf-8')

        free(c_error_message)

        return error_message


