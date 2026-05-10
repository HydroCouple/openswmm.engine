# cython: language_level=3str
# Description: Cython module for openswmmcore output file processing and data extraction functions for the openswmmcore python package.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# python and cython imports
import os
from enum import Enum
from typing import List, Tuple, Union, Optional, Dict, Set
from cpython.datetime cimport datetime, timedelta
from libc.stdlib cimport free, malloc

# external imports

# local python and cython imports
from ..solver import (
    decode_swmm_datetime,
)

from .output cimport (
    SMO_unitSystem,
    SMO_flowUnits,
    SMO_concUnits,
    SMO_elementType,
    SMO_time,
    SMO_subcatchAttribute,
    SMO_nodeAttribute,
    SMO_linkAttribute,
    SMO_systemAttribute,
    SMO_Handle,
    MAXFILENAME,
    MAXELENAME,
    SMO_init,
    SMO_open,
    SMO_close,
    SMO_getVersion,
    SMO_getProjectSize,
    SMO_getUnits,
    SMO_getFlowUnits,
    SMO_getPollutantUnits,
    SMO_getStartDate,
    SMO_getTimes,
    SMO_getElementName,
    SMO_getNumVars,
    SMO_getVarCode,
    SMO_getVarCodes,
    SMO_getNumProperties,
    SMO_getPropertyCode,
    SMO_getPropertyCodes,
    SMO_getPropertyValue,
    SMO_getPropertyValues,
    SMO_getSubcatchSeries,
    SMO_getNodeSeries,
    SMO_getLinkSeries,
    SMO_getSystemSeries,
    SMO_getSubcatchAttribute,
    SMO_getNodeAttribute,
    SMO_getLinkAttribute,
    SMO_getLinkAttribute,
    SMO_getSystemAttribute,
    SMO_getSubcatchResult,
    SMO_getNodeResult,
    SMO_getLinkResult,
    SMO_getSystemResult,
    SMO_free,
    SMO_clearError,
    SMO_checkError
)

class UnitSystem(Enum):
    """
    Enumeration of the unit system used in the output file.

    @ivar US: US customary units.
    @type US: int
    @ivar SI: SI metric units.
    @type SI: int
    """
    US = SMO_unitSystem.SMO_US #: US customary units.
    SI = SMO_unitSystem.SMO_SI #: SI metric units.

class FlowUnits(Enum):
    """
    Enumeration of the flow units used in the simulation.

    @ivar CFS: Cubic feet per second.
    @type CFS: int
    @ivar GPM: Gallons per minute.
    @type GPM: int
    @ivar MGD: Million gallons per day.
    @type MGD: int
    @ivar CMS: Cubic meters per second.
    @type CMS: int
    @ivar LPS: Liters per second.
    @type LPS: int
    @ivar MLD: Million liters per day.
    @type MLD: int
    """
    CFS = SMO_flowUnits.SMO_CFS #: Cubic feet per second.
    GPM = SMO_flowUnits.SMO_GPM #: Gallons per minute.
    MGD = SMO_flowUnits.SMO_MGD #: Million gallons per day.
    CMS = SMO_flowUnits.SMO_CMS #: Cubic meters per second.
    LPS = SMO_flowUnits.SMO_LPS #: Liters per second.
    MLD = SMO_flowUnits.SMO_MLD #: Million liters per day.

class ConcentrationUnits(Enum):
    """
    Enumeration of the concentration units used in the simulation.

    @ivar MG: Milligrams per liter (mg/L).
    @type MG: int
    @ivar UG: Micrograms per liter (ug/L).
    @type UG: int
    @ivar COUNT: Counts per liter (#/L).
    @type COUNT: int
    @ivar NONE: No concentration units (dimensionless).
    @type NONE: int
    """
    MG = SMO_concUnits.SMO_MG #: Milligrams per liter.
    UG = SMO_concUnits.SMO_UG #: Micrograms per liter.
    COUNT = SMO_concUnits.SMO_COUNT #: Counts per liter.
    NONE = SMO_concUnits.SMO_NONE #: No units.

class ElementType(Enum):
    """
    Enumeration of the SWMM element types stored in an output file.

    @ivar SUBCATCHMENT: Subcatchment element.
    @type SUBCATCHMENT: int
    @ivar NODE: Node element (junction, outfall, storage, or divider).
    @type NODE: int
    @ivar LINK: Link element (conduit, pump, orifice, weir, or outlet).
    @type LINK: int
    @ivar SYSTEM: System-level pseudo-element (one per simulation).
    @type SYSTEM: int
    @ivar POLLUTANT: Pollutant element (water quality constituent).
    @type POLLUTANT: int
    """
    SUBCATCHMENT = SMO_elementType.SMO_subcatch #: Subcatchment.
    NODE = SMO_elementType.SMO_node #: Node.
    LINK = SMO_elementType.SMO_link #: Link.
    SYSTEM = SMO_elementType.SMO_sys #: System.
    POLLUTANT = SMO_elementType.SMO_pollut #: Pollutant.

class TimeAttribute(Enum):
    """
    Enumeration of the report-time meta-attributes stored in the output file header.

    @ivar REPORT_STEP: Reporting time step size in seconds.
    @type REPORT_STEP: int
    @ivar NUM_PERIODS: Total number of reporting periods written to the file.
    @type NUM_PERIODS: int
    """
    REPORT_STEP = SMO_time.SMO_reportStep #: Report step size (seconds).
    NUM_PERIODS = SMO_time.SMO_numPeriods #: Number of reporting periods.

class SubcatchAttribute(Enum):
    """
    Enumeration of the subcatchment result attributes recorded per reporting period.

    @ivar RAINFALL: Subcatchment rainfall rate (in/hr or mm/hr).
    @type RAINFALL: int
    @ivar SNOW_DEPTH: Subcatchment snow depth (in or mm).
    @type SNOW_DEPTH: int
    @ivar EVAPORATION_LOSS: Subcatchment evaporation loss rate (in/hr or mm/hr).
    @type EVAPORATION_LOSS: int
    @ivar INFILTRATION_LOSS: Subcatchment infiltration loss rate (in/hr or mm/hr).
    @type INFILTRATION_LOSS: int
    @ivar RUNOFF_RATE: Subcatchment surface runoff rate (flow units).
    @type RUNOFF_RATE: int
    @ivar GROUNDWATER_OUTFLOW: Subcatchment groundwater outflow rate (flow units).
    @type GROUNDWATER_OUTFLOW: int
    @ivar GROUNDWATER_TABLE_ELEVATION: Subcatchment groundwater table elevation (ft or m).
    @type GROUNDWATER_TABLE_ELEVATION: int
    @ivar SOIL_MOISTURE: Subcatchment unsaturated zone soil moisture content (dimensionless fraction).
    @type SOIL_MOISTURE: int
    @ivar POLLUTANT_CONCENTRATION: Subcatchment runoff pollutant concentration (mass/volume).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_CONCENTRATION: int
    """
    RAINFALL = SMO_subcatchAttribute.SMO_rainfall_subcatch #: Subcatchment rainfall (in/hr or mm/hr).
    SNOW_DEPTH = SMO_subcatchAttribute.SMO_snow_depth_subcatch #: Subcatchment snow depth (in or mm).
    EVAPORATION_LOSS = SMO_subcatchAttribute.SMO_evap_loss #: Subcatchment evaporation loss (in/hr or mm/hr).
    INFILTRATION_LOSS = SMO_subcatchAttribute.SMO_infil_loss #: Subcatchment infiltration loss (in/hr or mm/hr).
    RUNOFF_RATE = SMO_subcatchAttribute.SMO_runoff_rate #: Subcatchment runoff flow (flow units).
    GROUNDWATER_OUTFLOW = SMO_subcatchAttribute.SMO_gwoutflow_rate #: Subcatchment groundwater flow (flow units).
    GROUNDWATER_TABLE_ELEVATION = SMO_subcatchAttribute.SMO_gwtable_elev #: Subcatchment groundwater elevation (ft or m).
    SOIL_MOISTURE = SMO_subcatchAttribute.SMO_soil_moisture #: Subcatchment soil moisture content (-).
    POLLUTANT_CONCENTRATION = SMO_subcatchAttribute.SMO_pollutant_conc_subcatch #: Subcatchment pollutant concentration (-).

class NodeAttribute(Enum):
    """
    Enumeration of the node result attributes recorded per reporting period.

    @ivar INVERT_DEPTH: Water depth above the node invert elevation (ft or m).
    @type INVERT_DEPTH: int
    @ivar HYDRAULIC_HEAD: Node hydraulic head (ft or m above datum).
    @type HYDRAULIC_HEAD: int
    @ivar STORED_VOLUME: Volume of water stored at the node (ft3 or m3).
    @type STORED_VOLUME: int
    @ivar LATERAL_INFLOW: Lateral (direct) inflow rate at the node (flow units).
    @type LATERAL_INFLOW: int
    @ivar TOTAL_INFLOW: Total inflow rate entering the node (flow units).
    @type TOTAL_INFLOW: int
    @ivar FLOODING_LOSSES: Flooding loss rate leaving the node (flow units).
    @type FLOODING_LOSSES: int
    @ivar POLLUTANT_CONCENTRATION: Node water quality pollutant concentration (mass/volume).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_CONCENTRATION: int
    """
    INVERT_DEPTH = SMO_nodeAttribute.SMO_invert_depth #: Node depth above invert (ft or m).
    HYDRAULIC_HEAD = SMO_nodeAttribute.SMO_hydraulic_head #: Node hydraulic head (ft or m).
    STORED_VOLUME = SMO_nodeAttribute.SMO_stored_ponded_volume #: Node volume stored (ft3 or m3).
    LATERAL_INFLOW = SMO_nodeAttribute.SMO_lateral_inflow #: Node lateral inflow (flow units).
    TOTAL_INFLOW = SMO_nodeAttribute.SMO_total_inflow #: Node total inflow (flow units).
    FLOODING_LOSSES = SMO_nodeAttribute.SMO_flooding_losses #: Node flooding losses (flow units).
    POLLUTANT_CONCENTRATION = SMO_nodeAttribute.SMO_pollutant_conc_node #: Node pollutant concentration (-).

class LinkAttribute(Enum):
    """
    Enumeration of the link result attributes recorded per reporting period.

    @ivar FLOW_RATE: Link flow rate (flow units).
    @type FLOW_RATE: int
    @ivar FLOW_DEPTH: Link flow depth (ft or m).
    @type FLOW_DEPTH: int
    @ivar FLOW_VELOCITY: Link flow velocity (ft/s or m/s).
    @type FLOW_VELOCITY: int
    @ivar FLOW_VOLUME: Link flow volume (ft3 or m3).
    @type FLOW_VOLUME: int
    @ivar CAPACITY: Link capacity as a fraction of full conduit depth (dimensionless, 0-1).
    @type CAPACITY: int
    @ivar POLLUTANT_CONCENTRATION: Link water quality pollutant concentration (mass/volume).
        Use C{sub_index} to select individual pollutants.
    @type POLLUTANT_CONCENTRATION: int
    """
    FLOW_RATE = SMO_linkAttribute.SMO_flow_rate_link #: Link flow rate (flow units).
    FLOW_DEPTH = SMO_linkAttribute.SMO_flow_depth #: Link flow depth (ft or m).
    FLOW_VELOCITY = SMO_linkAttribute.SMO_flow_velocity #: Link flow velocity (ft/s or m/s).
    FLOW_VOLUME = SMO_linkAttribute.SMO_flow_volume #: Link flow volume (ft3 or m3).
    CAPACITY = SMO_linkAttribute.SMO_capacity #: Link capacity (fraction of conduit filled).
    POLLUTANT_CONCENTRATION = SMO_linkAttribute.SMO_pollutant_conc_link #: Link pollutant concentration (-).

class SystemAttribute(Enum):
    """
    Enumeration of the system-level result attributes recorded per reporting period.

    @ivar AIR_TEMP: Air temperature (deg F or deg C).
    @type AIR_TEMP: int
    @ivar RAINFALL: System-wide rainfall intensity (in/hr or mm/hr).
    @type RAINFALL: int
    @ivar SNOW_DEPTH: System-wide snow depth (in or mm).
    @type SNOW_DEPTH: int
    @ivar EVAP_INFIL_LOSS: Combined evaporation and infiltration loss rate (in/day or mm/day).
    @type EVAP_INFIL_LOSS: int
    @ivar RUNOFF_FLOW: Total surface runoff flow rate (flow units).
    @type RUNOFF_FLOW: int
    @ivar DRY_WEATHER_INFLOW: Total dry weather inflow rate (flow units).
    @type DRY_WEATHER_INFLOW: int
    @ivar GROUNDWATER_INFLOW: Total groundwater inflow rate (flow units).
    @type GROUNDWATER_INFLOW: int
    @ivar RDII_INFLOW: Total Rainfall Derived Infiltration and Inflow rate (flow units).
    @type RDII_INFLOW: int
    @ivar DIRECT_INFLOW: Total direct inflow rate (flow units).
    @type DIRECT_INFLOW: int
    @ivar TOTAL_LATERAL_INFLOW: Sum of dry weather, groundwater, RDII, and direct inflows (flow units).
    @type TOTAL_LATERAL_INFLOW: int
    @ivar FLOOD_LOSSES: Total system-wide flooding loss rate (flow units).
    @type FLOOD_LOSSES: int
    @ivar OUTFALL_FLOWS: Total outfall discharge rate (flow units).
    @type OUTFALL_FLOWS: int
    @ivar VOLUME_STORED: Total volume stored in all storage nodes (ft3 or m3).
    @type VOLUME_STORED: int
    @ivar EVAPORATION_RATE: Actual evaporation rate (in/day or mm/day).
    @type EVAPORATION_RATE: int
    """
    AIR_TEMP = SMO_systemAttribute.SMO_air_temp #: Air temperature (deg. F or deg. C).
    RAINFALL = SMO_systemAttribute.SMO_rainfall_system #: Rainfall intensity (in/hr or mm/hr).
    SNOW_DEPTH = SMO_systemAttribute.SMO_snow_depth_system #: Snow depth (in or mm).
    EVAP_INFIL_LOSS = SMO_systemAttribute.SMO_evap_infil_loss #: Evaporation and infiltration loss rate (in/day or mm/day).
    RUNOFF_FLOW = SMO_systemAttribute.SMO_runoff_flow #: Runoff flow (flow units).
    DRY_WEATHER_INFLOW = SMO_systemAttribute.SMO_dry_weather_inflow #: Dry weather inflow (flow units).
    GROUNDWATER_INFLOW = SMO_systemAttribute.SMO_groundwater_inflow #: Groundwater inflow (flow units).
    RDII_INFLOW = SMO_systemAttribute.SMO_RDII_inflow #: Rainfall Derived Infiltration and Inflow (RDII) (flow units).
    DIRECT_INFLOW = SMO_systemAttribute.SMO_direct_inflow #: Direct inflow (flow units).
    TOTAL_LATERAL_INFLOW = SMO_systemAttribute.SMO_total_lateral_inflow #: Total lateral inflow; sum of variables 4 to 8 (flow units).
    FLOOD_LOSSES = SMO_systemAttribute.SMO_flood_losses #: Flooding losses (flow units).
    OUTFALL_FLOWS = SMO_systemAttribute.SMO_outfall_flows #: Outfall flow (flow units).
    VOLUME_STORED = SMO_systemAttribute.SMO_volume_stored #: Volume stored in storage nodes (ft3 or m3).
    EVAPORATION_RATE = SMO_systemAttribute.SMO_evap_rate #: Evaporation rate (in/day or mm/day).

class SWMMOutputException(Exception):
    """
    Exception raised when an error occurs while reading a SWMM binary output file.

    @param message: Human-readable description of the error condition.
    @type message: str
    """
    def __init__(self, message: str) -> None:
        """
        Initialise the exception with a descriptive error message.

        @param message: Human-readable description of the error condition.
        @type message: str
        """
        super().__init__(message)

cdef class Output:
    """
    Reader for the binary output file (``.out``) produced by a SWMM simulation.

    On construction the file is opened and key metadata (version, unit system,
    element counts, start date, and reporting periods) is read and cached.
    All subsequent accessors use the cached values or call the underlying C API
    as needed.  Use as a context manager to guarantee the file handle is closed::

        with Output('simulation.out') as out:
            ts = out.get_node_timeseries(0, NodeAttribute.INVERT_DEPTH)
            for dt, value in ts.items():
                print(dt, value)

    @cvar _output_file_handle: Internal C-level handle to the open SWMM output file.
    @cvar _version: SWMM version number recorded in the output-file header.
    @cvar _units: Raw C integer array of unit codes
        (index 0 = unit system, index 1 = flow units, index 2+ = pollutant units).
    @cvar _flow_units: Cached integer code for the active flow-unit system.
    @cvar _output_size: Cached C integer array of element counts indexed by
        C{ElementType} value (subcatchments, nodes, links, system, pollutants).
    @cvar _pollutant_units: Cached C{ConcentrationUnits} list, one entry per pollutant.
    @cvar _start_date: Simulation start date decoded from the output-file header.
    @cvar _report_step: Reporting time step in seconds.
    @cvar _num_periods: Total number of reporting periods stored in the file.
    @cvar _times: Pre-computed list of C{datetime} objects for every reporting period.
    """
    cdef SMO_Handle _output_file_handle
    cdef int _version
    cdef int* _units
    cdef int _units_length
    cdef int _flow_units
    cdef int* _output_size
    cdef int _output_size_length
    cdef list _pollutant_units
    cdef dict _element_name_indexes
    cdef datetime _start_date
    cdef int _report_step
    cdef int _num_periods
    cdef list _times

    # ====================================================================
    # File lifecycle
    # ====================================================================

    def __cinit__(self, str output_file):
        """
        Open and parse the SWMM binary output file, caching all header metadata.

        @param output_file: Absolute or relative path to the SWMM ``.out`` file.
        @type output_file: str
        @raise FileNotFoundError: If no file exists at C{output_file}.
        @raise SWMMOutputException: If the SWMM output API initialisation or
            file-open call returns a non-zero error code.
        """
        cdef int i = 0
        cdef int error_code =  0
        cdef bytes path_bytes = output_file.encode('utf-8')
        cdef const char* c_output_file = path_bytes
        self._output_file_handle = NULL

        self._output_size = NULL
        self._units = NULL

        error_code =  SMO_init(&self._output_file_handle)

        # get error message if error code is not 0 and print it and prevent any memory leaks
        if error_code != 0:
            # create a buffer to store the error message
            error_message = self.check_error()
            raise SWMMOutputException(f"Error initializing the SWMM output file {output_file}. Error code: {error_code}: {error_message}")

        # Check if the output file exists
        if not os.path.exists(output_file):
            raise FileNotFoundError(f"Error opening the SWMM output file {output_file}. Error code: 434: The output file does not exist.")

        error_code = SMO_open(self._output_file_handle, c_output_file)

        # get error message if error code is not 0 and print it and prevent any memory leaks
        if error_code != 0:
            # create a buffer to store the error message
            error_message = self.check_error()

            if error_code > 400:
                self._output_file_handle = NULL

            if error_code == 434:
                raise FileNotFoundError(f"Error opening the SWMM output file {output_file}. Error code: {error_code}: {error_message}. The output file may not exist or may be locked by another process.")
            else:
                raise SWMMOutputException(f"Error opening the SWMM output file {output_file}. Error code: {error_code}: {error_message}")

        # Read and cache output attributes for faster access
        self._version = self.__get_version()
        self._units, self._units_length = self.__get_units()
        self._flow_units = self.__get_flow_units()
        self._output_size, self._output_size_length = self.__get_output_size()
        self._pollutant_units = [ConcentrationUnits(i) for i in  self.__get_pollutant_units()]
        self._element_name_indexes = self.__get_element_name_indexes()
        self._start_date = self.__get_start_date()
        self._report_step = self.get_time_attribute(TimeAttribute.REPORT_STEP)
        self._num_periods = self.get_time_attribute(TimeAttribute.NUM_PERIODS)
        self._times = [self._start_date + timedelta(seconds=self._report_step) * i for i in range(1, self._num_periods + 1)]

    def __enter__(self):
        """
        Enter the runtime context; returns this C{Output} instance.

        @return: This C{Output} instance, ready for metadata and time-series access.
        @rtype: Output
        """
        return self

    def __close(self):
        """
        Release all C-level resources held by this instance.

        Closes the SWMM output file handle and frees the cached C integer
        arrays for output size and unit codes.  Safe to call multiple times.
        """
        if self._output_file_handle != NULL:
            SMO_close(&self._output_file_handle)
            self._output_file_handle = NULL

        if self._output_size != NULL:
            free(self._output_size)
            self._output_size = NULL

        if self._units != NULL:
            free(self._units)
            self._units = NULL

    def __exit__(self, exc_type, exc_value, traceback):
        """
        Exit the runtime context, releasing all C-level output file resources.

        Delegates to the internal close routine regardless of whether an
        exception was raised inside the C{with} block.

        @param exc_type: Exception type, or C{None} if no exception was raised.
        @param exc_value: Exception instance, or C{None}.
        @param traceback: Traceback object, or C{None}.
        """
        self.__close()

    def __dealloc__(self):
        """
        Cython destructor — free all C-level resources when the object is garbage-collected.

        Delegates to the internal close routine to release the file handle and
        any malloc'd arrays, preventing memory leaks.
        """
        self.__close()

    # ====================================================================
    # Metadata
    # ====================================================================

    @property
    def version(self) -> int:
        """
        SWMM engine version number recorded in the output file header.

        @return: Integer version number (e.g., C{51017} for SWMM 5.1.017).
        @rtype: int
        """
        return self._version

    cdef int __get_version(self):
        """
        Read the SWMM version from the output file via the C API.

        @return: Integer version number.
        @rtype: int
        @raise SWMMOutputException: If C{SMO_getVersion} returns a non-zero error code.
        """
        cdef int error_code = 0
        cdef int version = 0

        error_code = SMO_getVersion(self._output_file_handle, &version)
        self.__validate_error_code(error_code)

        return version

    @property
    def output_size(self) -> Dict[str, int]:
        """
        Element counts for each object category stored in the output file.

        @return: Dictionary with keys C{'subcatchments'}, C{'nodes'},
            C{'links'}, C{'system'}, and C{'pollutants'} mapped to their
            respective element counts.
        @rtype: Dict[str, int]
        """
        cdef dict output_size_dict = {
            'subcatchments': self._output_size[0],
            'nodes': self._output_size[1],
            'links': self._output_size[2],
            'system': self._output_size[3],
            'pollutants': self._output_size[4]
        }

        return output_size_dict

    cdef (int*, int) __get_output_size(self):
        """
        Read element counts from the output file header via the C API.

        @return: A 2-tuple of (C integer pointer to the size array, array length).
        @rtype: tuple
        @raise SWMMOutputException: If C{SMO_getProjectSize} returns a non-zero error code.
        """
        cdef int error_code = 0
        cdef int *project_size = NULL
        cdef int length = 0

        error_code = SMO_getProjectSize(self._output_file_handle, &project_size, &length)
        self.__validate_error_code(error_code)

        return project_size, length

    @property
    def units(self) -> Tuple[UnitSystem, FlowUnits, Optional[List[ConcentrationUnits]]]:
        """
        All unit systems recorded in the output file as a single tuple.

        @return: A 3-tuple of (C{UnitSystem}, C{FlowUnits}, list of
            C{ConcentrationUnits} — one per pollutant, or an empty list when
            no pollutants are present).
        @rtype: Tuple[UnitSystem, FlowUnits, Optional[List[ConcentrationUnits]]]
        """
        return (
            UnitSystem(self._units[0]),
            FlowUnits(self._units[1]),
            [ConcentrationUnits(self._units[i]) for i in range(2, self._units_length)]
        )

    cdef (int*, int) __get_units(self):
        """
        Read the unit-code array from the output file header via the C API.

        @return: A 2-tuple of (C integer pointer to the units array, array length).
        @rtype: tuple
        @raise SWMMOutputException: If C{SMO_getUnits} returns a non-zero error code.
        """
        cdef int error_code = 0
        cdef int *units = NULL
        cdef int length = 0

        error_code = SMO_getUnits(self._output_file_handle, &units, &length)
        self.__validate_error_code(error_code)

        return units, length

    @property
    def flow_units(self) -> FlowUnits:
        """
        Flow unit system active throughout the simulation.

        @return: C{FlowUnits} enumeration member describing the flow units.
        @rtype: FlowUnits
        """
        return FlowUnits(self._flow_units)

    cdef int __get_flow_units(self):
        """
        Read the flow-unit integer code from the output file header via the C API.

        @return: Integer flow-unit code.
        @rtype: int
        @raise SWMMOutputException: If C{SMO_getFlowUnits} returns a non-zero error code.
        """
        cdef int error_code = 0
        cdef int flow_units = 0

        error_code = SMO_getFlowUnits(self._output_file_handle, &flow_units)
        self.__validate_error_code(error_code)

        return flow_units

    @property
    def pollutant_units(self) -> List[ConcentrationUnits]:
        """
        Concentration unit for each pollutant modelled in the simulation.

        @return: List of C{ConcentrationUnits} members ordered by pollutant
            index.  Returns an empty list when no pollutants are present.
        @rtype: List[ConcentrationUnits]
        """
        return self._pollutant_units

    cdef list __get_pollutant_units(self):
        """
        Read pollutant concentration-unit codes from the output file via the C API.

        @return: Python list of raw integer concentration-unit codes (one per pollutant).
        @rtype: list
        @raise SWMMOutputException: If C{SMO_getPollutantUnits} returns a non-zero error code.
        """
        cdef int i = 0
        cdef int error_code = 0
        cdef int *pollutant_units = NULL
        cdef int length = 0
        cdef list pollutant_units_list = []

        error_code = SMO_getPollutantUnits(self._output_file_handle, &pollutant_units, &length)
        self.__validate_error_code(error_code)

        pollutant_units_list = [pollutant_units[i] for i in range(length)]

        if pollutant_units != NULL:
            free(pollutant_units)

        return pollutant_units_list

    cdef dict __get_element_name_indexes(self):
        """
        Build a name-to-index lookup dictionary for every element type except SYSTEM.

        @return: Nested dict mapping element type name to a dict of
            {element_name: element_index} pairs.
        @rtype: Dict[str, Dict[str, int]]
        """
        cdef dict element_name_indexes = {}

        for element_type in ElementType:
            if element_type.value == SMO_elementType.SMO_sys:
                continue

            num_elements = self._output_size[element_type.value]
            element_names = self.get_element_names(element_type)

            element_name_indexes[element_type.name] = {element_name: i for i , element_name in enumerate(element_names)}

        return element_name_indexes

    @property
    def start_date(self) -> datetime:
        """
        Simulation start date and time decoded from the output file header.

        @return: Start date/time of the simulation as a C{datetime} object.
        @rtype: datetime
        """
        return self._start_date

    cdef datetime __get_start_date(self):
        """
        Read and decode the simulation start date from the output file via the C API.

        @return: Start date/time decoded from the SWMM double-precision date value.
        @rtype: datetime
        @raise SWMMOutputException: If C{SMO_getStartDate} returns a non-zero error code.
        """
        cdef double swmm_datetime = 0

        error_code = SMO_getStartDate(self._output_file_handle, &swmm_datetime)
        self.__validate_error_code(error_code)

        return decode_swmm_datetime(swmm_datetime)

    @property
    def times(self) -> List[datetime]:
        """
        Datetime object for every reporting period stored in the output file.

        The list begins at C{start_date + report_step} and contains exactly
        C{num_periods} entries, one per reporting interval.

        @return: Ordered list of reporting-period datetimes.
        @rtype: List[datetime]
        """
        return self._times

    # ====================================================================
    # Variable metadata
    # ====================================================================

    def get_num_variables(self, element_type: ElementType) -> int:
        """
        Return the number of result variables recorded for an element type.

        @param element_type: The SWMM element category to query.
        @type element_type: ElementType
        @return: Count of result variables stored per reporting period for
            C{element_type}.
        @rtype: int
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_vars = 0

        error_code = SMO_getNumVars(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            count=&num_vars
        )

        self.__validate_error_code(error_code)

        return num_vars

    def get_variable_code(self, element_type: ElementType, variable_index: int) -> int:
        """
        Return the integer code for a single result variable.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @param variable_index: Zero-based index of the variable within the
            element type's variable list.
        @type variable_index: int
        @return: Integer variable code.
        @rtype: int
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int var_code = 0

        error_code = SMO_getVarCode(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            varIndex=variable_index,
            varCode=&var_code
        )

        self.__validate_error_code(error_code)

        return var_code

    def get_variable_codes(self, element_type: ElementType) -> List[int]:
        """
        Return all result variable codes for an element type.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @return: List of integer variable codes in variable-index order.
        @rtype: List[int]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_vars = 0
        cdef int* var_codes = NULL
        cdef list variable_codes

        error_code = SMO_getVarCodes(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            varCodes=&var_codes,
            size=&num_vars
        )

        self.__validate_error_code(error_code)

        variable_codes = [var_codes[i] for i in range(num_vars)]

        if var_codes != NULL:
            free(var_codes)

        return variable_codes

    def get_num_properties(self, element_type: ElementType) -> int:
        """
        Return the number of static properties recorded for an element type.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @return: Count of static (time-invariant) properties for C{element_type}.
        @rtype: int
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_properties = 0

        error_code = SMO_getNumProperties(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            count=&num_properties
        )

        self.__validate_error_code(error_code)

        return num_properties

    def get_property_code(self, element_type: ElementType, property_index: int) -> int:
        """
        Return the integer code for a single static property.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @param property_index: Zero-based index of the property within the
            element type's property list.
        @type property_index: int
        @return: Integer property code.
        @rtype: int
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int property_code = 0

        error_code = SMO_getPropertyCode(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            propertyIndex=property_index,
            propertyCode=&property_code
        )

        self.__validate_error_code(error_code)

        return property_code

    def get_property_codes(self, element_type: ElementType) -> List[int]:
        """
        Return all static property codes for an element type.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @return: List of integer property codes in property-index order.
        @rtype: List[int]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_properties = 0
        cdef int* property_codes = NULL

        error_code = SMO_getPropertyCodes(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            propertyCodes=&property_codes,
            size=&num_properties
        )

        self.__validate_error_code(error_code)

        property_codes_list = [property_codes[i] for i in range(num_properties)]

        if property_codes != NULL:
            free(property_codes)

        return property_codes_list

    def get_property_value(self, element_type: ElementType, element_index: Union[int, str], property_code: int) -> float:
        """
        Return a single static property value for a specific element.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @param element_index: Zero-based element index or element name string.
        @type element_index: Union[int, str]
        @param property_code: Integer property code obtained from C{get_property_code}.
        @type property_code: int
        @return: Property value in the units recorded in the output file.
        @rtype: float
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float property_value = 0
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.NODE.name][element_index]

        error_code = SMO_getPropertyValue(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            propertyIndex=property_code,
            elementIndex=l_element_index,
            value=&property_value
        )

        self.__validate_error_code(error_code)

        return property_value

    def get_property_values(self, element_type: ElementType, element_index: Union[int, str]) -> List[float]:
        """
        Return all static property values for a specific element.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @param element_index: Zero-based element index or element name string.
        @type element_index: Union[int, str]
        @return: Ordered list of all property values for the element.
        @rtype: List[float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_elements = 0
        cdef int i = 0
        cdef float* property_values = NULL
        cdef list property_values_list
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.NODE.name][element_index]

        error_code = SMO_getPropertyValues(
            p_handle=self._output_file_handle,
            type=<SMO_elementType>element_type.value,
            elementIndex=l_element_index,
            outValueArray=&property_values,
            length=&num_elements
        )

        self.__validate_error_code(error_code)
        property_values_list = [*<float[:num_elements]>property_values]

        if property_values != NULL:
            free(property_values)

        return property_values_list

    # ====================================================================
    # Temporal attributes
    # ====================================================================

    def get_time_attribute(self, time_attribute: TimeAttribute) -> int:
        """
        Return a temporal meta-attribute from the output file header.

        @param time_attribute: The temporal attribute to retrieve
            (C{REPORT_STEP} returns step size in seconds;
            C{NUM_PERIODS} returns the total period count).
        @type time_attribute: TimeAttribute
        @return: Integer value of the requested temporal attribute.
        @rtype: int
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int temporal_attribute = -1

        error_code = SMO_getTimes(self._output_file_handle, <SMO_time>time_attribute.value, &temporal_attribute)
        self.__validate_error_code(error_code)

        return temporal_attribute

    # ====================================================================
    # Element names
    # ====================================================================

    def get_element_name(self, element_type: ElementType,  element_index: int) -> str:
        """
        Return the name of a single SWMM element as recorded in the input file.

        @param element_type: The SWMM element category.
        @type element_type: ElementType
        @param element_index: Zero-based index of the element.
        @type element_index: int
        @return: Element name string decoded from UTF-8.
        @rtype: str
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int strlen = 0
        cdef char* element_name = NULL

        error_code = SMO_getElementName(self._output_file_handle, <SMO_elementType>element_type.value, element_index, &element_name, &strlen)
        self.__validate_error_code(error_code)

        # Convert the C string to a Python string and delete the C string
        element_name_str = element_name.decode('utf-8')

        if element_name != NULL:
            free(element_name)

        return element_name_str

    def get_element_names(self, element_type: ElementType) -> List[str]:
        """
        Return the names of all elements of the given type.

        @param element_type: The SWMM element category.  May not be
            C{ElementType.SYSTEM}, which has no named instances.
        @type element_type: ElementType
        @return: Ordered list of element name strings (index matches element index).
        @rtype: List[str]
        @raise SWMMOutputException: If C{element_type} is C{SYSTEM} or the
            underlying C API call fails.
        """
        cdef int error_code = 0
        cdef int num_elements = 0
        cdef int i = 0
        cdef int strlen = 0
        cdef char** c_element_names = NULL
        cdef list element_names

        if element_type.value == SMO_elementType.SMO_sys:
            raise SWMMOutputException(f"Cannot get element names for the system element type {ElementType.SYSTEM}.")
        elif element_type.value > SMO_elementType.SMO_pollut:
            raise SWMMOutputException("Invalid element type.")

        num_elements = self._output_size[element_type.value]

        c_element_names = <char**>malloc(num_elements * sizeof(char*))

        for i in range(num_elements):
            error_code = SMO_getElementName(self._output_file_handle, <SMO_elementType>element_type.value, i, &c_element_names[i], &strlen)
            self.__validate_error_code(error_code)

        element_names = [c_element_names[i].decode('utf-8') for i in range(num_elements)]

        if c_element_names != NULL:
            for i in range(num_elements):
                if c_element_names[i] != NULL:
                    free(c_element_names[i])
                    c_element_names[i] = NULL

            free(c_element_names)

        return element_names

    # ====================================================================
    # Per-element time series
    # ====================================================================

    def get_subcatchment_timeseries(
        self, element_index: Union[int, str],
        attribute: SubcatchAttribute,
        start_date_index: int = 0,
        end_date_index: int = -1,
        sub_index: int = 0
        ) -> Dict[datetime, float]:
        """
        Return a time series for a subcatchment result attribute.

        @param element_index: Zero-based subcatchment index or subcatchment name string.
        @type element_index: Union[int, str]
        @param attribute: The subcatchment result attribute to extract.
        @type attribute: SubcatchAttribute
        @param start_date_index: Index into C{times} for the first period to
            retrieve (inclusive).  Defaults to C{0}.
        @type start_date_index: int
        @param end_date_index: Index into C{times} for the last period to
            retrieve (exclusive).  Use C{-1} to retrieve through the final period.
            Defaults to C{-1}.
        @type end_date_index: int
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Ordered mapping from reporting-period C{datetime} to attribute value.
        @rtype: Dict[datetime, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.SUBCATCHMENT.name][element_index]

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getSubcatchSeries(
            p_handle=self._output_file_handle,
            subcatchIndex=l_element_index,
            attr=<SMO_subcatchAttribute>attribute_index,
            startPeriod=start_date_index,
            endPeriod=end_date_index,
            outValueArray=&values,
            length=&length
        )
        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_node_timeseries(
        self,
        element_index: Union[int, str],
        attribute: NodeAttribute,
        start_date_index: int = 0,
        end_date_index: int = -1,
        sub_index: int = 0
        ) -> Dict[datetime, float]:
        """
        Return a time series for a node result attribute.

        @param element_index: Zero-based node index or node name string.
        @type element_index: Union[int, str]
        @param attribute: The node result attribute to extract.
        @type attribute: NodeAttribute
        @param start_date_index: Index into C{times} for the first period to
            retrieve (inclusive).  Defaults to C{0}.
        @type start_date_index: int
        @param end_date_index: Index into C{times} for the last period to
            retrieve (exclusive).  Use C{-1} to retrieve through the final period.
            Defaults to C{-1}.
        @type end_date_index: int
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Ordered mapping from reporting-period C{datetime} to attribute value.
        @rtype: Dict[datetime, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.NODE.name][element_index]

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getNodeSeries(
            p_handle=self._output_file_handle,
            nodeIndex=l_element_index,
            attr=<SMO_nodeAttribute>attribute_index,
            startPeriod=start_date_index,
            endPeriod=end_date_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_link_timeseries(
        self,
        element_index: Union[int, str],
        attribute: LinkAttribute,
        start_date_index: int = 0,
        end_date_index: int = -1,
        sub_index: int = 0
        ) -> Dict[datetime, float]:
        """
        Return a time series for a link result attribute.

        @param element_index: Zero-based link index or link name string.
        @type element_index: Union[int, str]
        @param attribute: The link result attribute to extract.
        @type attribute: LinkAttribute
        @param start_date_index: Index into C{times} for the first period to
            retrieve (inclusive).  Defaults to C{0}.
        @type start_date_index: int
        @param end_date_index: Index into C{times} for the last period to
            retrieve (exclusive).  Use C{-1} to retrieve through the final period.
            Defaults to C{-1}.
        @type end_date_index: int
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Ordered mapping from reporting-period C{datetime} to attribute value.
        @rtype: Dict[datetime, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.LINK.name][element_index]

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getLinkSeries(
            p_handle=self._output_file_handle,
            linkIndex=l_element_index,
            attr=<SMO_linkAttribute>(attribute_index),
            startPeriod=start_date_index,
            endPeriod=end_date_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_system_timeseries(
        self,
        attribute: SystemAttribute,
        start_date_index: int = 0,
        end_date_index: int = -1,
        sub_index: int = 0
        ) -> Dict[datetime, float]:
        """
        Return a time series for a system-level result attribute.

        @param attribute: The system result attribute to extract.
        @type attribute: SystemAttribute
        @param start_date_index: Index into C{times} for the first period to
            retrieve (inclusive).  Defaults to C{0}.
        @type start_date_index: int
        @param end_date_index: Index into C{times} for the last period to
            retrieve (exclusive).  Use C{-1} to retrieve through the final period.
            Defaults to C{-1}.
        @type end_date_index: int
        @param sub_index: Offset added to C{attribute.value} for extended
            attribute access.  Defaults to C{0}.
        @type sub_index: int
        @return: Ordered mapping from reporting-period C{datetime} to attribute value.
        @rtype: Dict[datetime, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getSystemSeries(
            p_handle=self._output_file_handle,
            attr=<SMO_systemAttribute>attribute_index,
            startPeriod=start_date_index,
            endPeriod=end_date_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    # ====================================================================
    # Bulk reads — all elements at one time step
    # ====================================================================

    def get_subcatchment_values_by_time_and_attribute(
        self,
        time_index: int,
        attribute: SubcatchAttribute,
        sub_index: int = 0
        ) -> Dict[str, float]:
        """
        Return a single result attribute for all subcatchments at one reporting step.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param attribute: The subcatchment result attribute to extract.
        @type attribute: SubcatchAttribute
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Mapping from subcatchment name to attribute value for every
            subcatchment in the model.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index

        error_code = SMO_getSubcatchAttribute(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            attr=<SMO_subcatchAttribute>attribute_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        subcatchment_values = dict(zip(self.get_element_names(ElementType.SUBCATCHMENT), <float[:length]>values))

        if values != NULL:
            free(values)

        return subcatchment_values

    def get_node_values_by_time_and_attribute(
        self,
        time_index: int,
        attribute: NodeAttribute,
        sub_index: int = 0
        ) -> Dict[str, float]:
        """
        Return a single result attribute for all nodes at one reporting step.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param attribute: The node result attribute to extract.
        @type attribute: NodeAttribute
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Mapping from node name to attribute value for every node in
            the model.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index

        error_code = SMO_getNodeAttribute(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            attr=<SMO_nodeAttribute>attribute_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        node_values = dict(zip(self.get_element_names(ElementType.NODE), <float[:length]>values))

        if values != NULL:
            free(values)

        return node_values

    def get_link_values_by_time_and_attribute(
        self,
        time_index: int,
        attribute: LinkAttribute,
        sub_index: int = 0
        ) -> Dict[str, float]:
        """
        Return a single result attribute for all links at one reporting step.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param attribute: The link result attribute to extract.
        @type attribute: LinkAttribute
        @param sub_index: Offset added to C{attribute.value} to select a
            specific pollutant when C{attribute} is C{POLLUTANT_CONCENTRATION}.
            Defaults to C{0}.
        @type sub_index: int
        @return: Mapping from link name to attribute value for every link in
            the model.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index

        error_code = SMO_getLinkAttribute(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            attr=<SMO_linkAttribute>attribute_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        link_values = dict(zip(self.get_element_names(ElementType.LINK), <float[:length]>values))

        if values != NULL:
            free(values)

        return link_values

    def get_system_values_by_time_and_attribute(
        self,
        time_index: int,
        attribute: SystemAttribute,
        sub_index: int = 0
        ) -> Dict[str, float]:
        """
        Return a single system attribute value at one reporting step.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param attribute: The system result attribute to extract.
        @type attribute: SystemAttribute
        @param sub_index: Offset added to C{attribute.value} for extended
            attribute access.  Defaults to C{0}.
        @type sub_index: int
        @return: Single-entry mapping from the C{SystemAttribute} name to its value.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int attribute_index = attribute.value + sub_index

        error_code = SMO_getSystemAttribute(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            attr=<SMO_systemAttribute>attribute_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        system_values = dict(zip([SystemAttribute(attribute).name], <float[:length]>values))


        if values != NULL:
            free(values)

        return system_values

    # ====================================================================
    # Convenience helpers — all attributes for one element at one time step
    # ====================================================================

    def get_subcatchment_values_by_time_and_element_index(
        self,
        time_index: int,
        element_index: Union[int, str]
        ) -> Dict[str, float]:
        """
        Return all result attributes for a single subcatchment at one reporting step.

        Enumerated attributes are keyed by their C{SubcatchAttribute} name;
        pollutant entries are keyed by the pollutant element name.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param element_index: Zero-based subcatchment index or subcatchment name string.
        @type element_index: Union[int, str]
        @return: Mapping from attribute/pollutant name to result value for all
            attributes of the specified subcatchment.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(SubcatchAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.SUBCATCHMENT.name][element_index]

        error_code = SMO_getSubcatchResult(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            subcatchIndex=l_element_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        for i in range(length):
            if i < enum_values_length:
                attrib_names.append(SubcatchAttribute(i).name)
            else:
                attrib_names.append(pollutant_names[i - enum_values_length])

        subcatchment_values = dict(zip(attrib_names, <float[:length]>values))

        if values != NULL:
            free(values)

        return subcatchment_values

    def get_node_values_by_time_and_element_index(
        self,
        time_index: int,
        element_index: Union[int, str]
        ) -> Dict[str, float]:
        """
        Return all result attributes for a single node at one reporting step.

        Enumerated attributes are keyed by their C{NodeAttribute} name;
        pollutant entries are keyed by the pollutant element name.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param element_index: Zero-based node index or node name string.
        @type element_index: Union[int, str]
        @return: Mapping from attribute/pollutant name to result value for all
            attributes of the specified node.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(NodeAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.NODE.name][element_index]

        error_code = SMO_getNodeResult(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            nodeIndex=l_element_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        for i in range(length):
            if i < enum_values_length:
                attrib_names.append(NodeAttribute(i).name)
            else:
                attrib_names.append(pollutant_names[i - enum_values_length])

        node_values = dict(zip(attrib_names, <float[:length]>values))

        if values != NULL:
            free(values)

        return node_values

    def get_link_values_by_time_and_element_index(
        self,
        time_index: int,
        element_index: Union[int, str]
        ) -> Dict[str, float]:
        """
        Return all result attributes for a single link at one reporting step.

        Enumerated attributes are keyed by their C{LinkAttribute} name;
        pollutant entries are keyed by the pollutant element name.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @param element_index: Zero-based link index or link name string.
        @type element_index: Union[int, str]
        @return: Mapping from attribute/pollutant name to result value for all
            attributes of the specified link.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(LinkAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []
        cdef int l_element_index = element_index if isinstance(element_index, int) \
                                   else self._element_name_indexes[ElementType.LINK.name][element_index]

        error_code = SMO_getLinkResult(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            linkIndex=l_element_index,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        for i in range(length):
            if i < enum_values_length:
                attrib_names.append(LinkAttribute(i).name)
            else:
                attrib_names.append(pollutant_names[i - enum_values_length])

        link_values = dict(zip(attrib_names, <float[:length]>values))

        if values != NULL:
            free(values)

        return link_values

    def get_system_values_by_time(self, time_index: int) -> Dict[str, float]:
        """
        Return all system-level result attributes at one reporting step.

        Each key is the C{SystemAttribute} member name for the corresponding variable.

        @param time_index: Zero-based index into the reporting period list.
        @type time_index: int
        @return: Mapping from system attribute name to result value, covering
            all C{SystemAttribute} members.
        @rtype: Dict[str, float]
        @raise SWMMOutputException: If the underlying C API call fails.
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(SystemAttribute) - 1

        error_code = SMO_getSystemResult(
            p_handle=self._output_file_handle,
            timeIndex=time_index,
            dummyIndex=0,
            outValueArray=&values,
            length=&length
        )

        self.__validate_error_code(error_code)

        system_values = dict(zip([SystemAttribute(i).name for i in range(enum_values_length)], <float[:length]>values))

        if values != NULL:
            free(values)

        return system_values

    # ====================================================================
    # Internal helpers (not part of the public API)
    # ====================================================================

    cdef str check_error(self):
        """
        Retrieve the current error message from the SWMM output API.

        @return: Error message string decoded from UTF-8, or an empty string
            if no error is pending.
        @rtype: str
        """
        cdef char* msg_buffer = NULL
        cdef int error_code

        error_code = SMO_checkError(self._output_file_handle, &msg_buffer)

        if error_code != 0 and msg_buffer != NULL:
            # Convert the C string to a Python string
            error_message = msg_buffer.decode('utf-8')
            # Free the allocated memory for the message buffer
            free(msg_buffer)
            return error_message
        else:
            return u""

    cdef str __validate_error_code(self, int error_code):
        """
        Validate a C API return code and raise C{SWMMOutputException} if non-zero.

        @param error_code: Integer error code returned by a C API function.
        @type error_code: int
        @raise SWMMOutputException: If C{error_code} is non-zero, including the
            human-readable error message retrieved from the API.
        """

        if error_code != 0:
            error_message = self.check_error()
            raise SWMMOutputException(f"Error code: {error_code}: {error_message}")
        else:
            return u""
