# Description: Cython module for epaswmm output file processing and data extraction functions for the epaswmm python package.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# cython: language_level=3

# python and cython imports
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

    :ivar US: US customary units.
    :type US: int
    :ivar SI: SI metric units.
    :type SI: int
    """
    US = SMO_unitSystem.SMO_US #: US customary units.
    SI = SMO_unitSystem.SMO_SI #: SI metric units.

class FlowUnits(Enum):
    """
    Enumeration of the flow units used in the simulation.

    :ivar CFS: Cubic feet per second.
    :type CFS: int
    :ivar GPM: Gallons per minute.
    :type GPM: int
    :ivar MGD: Million gallons per day.
    :type MGD: int
    :ivar CMS: Cubic meters per second.
    :type CMS: int
    :ivar LPS: Liters per second.
    :type LPS: int
    :ivar MLD: Million liters per day.
    :type MLD: int
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

    :ivar MG: Milligrams per liter.
    :type MG: int
    :ivar UG: Micrograms per liter.
    :type UG: int
    :ivar COUNT: Counts per liter.
    :type COUNT: int
    :ivar NONE: No units.
    :type NONE: int
    """
    MG = SMO_concUnits.SMO_MG #: Milligrams per liter.
    UG = SMO_concUnits.SMO_UG #: Micrograms per liter.
    COUNT = SMO_concUnits.SMO_COUNT #: Counts per liter.
    NONE = SMO_concUnits.SMO_NONE #: No units.

class ElementType(Enum):
    """
    Enumeration of the SWMM element types.

    :ivar SUBCATCHMENT: Subcatchment.
    :type SUBCATCHMENT: int
    :ivar NODE: Node.
    :type NODE: int
    :ivar LINK: Link.
    :type LINK: int
    :ivar SYS: System.
    :type SYS: int
    :ivar POLLUTANT: Pollutant.
    :type POLLUTANT: int
    """
    SUBCATCHMENT = SMO_elementType.SMO_subcatch #: Subcatchment.
    NODE = SMO_elementType.SMO_node #: Node.
    LINK = SMO_elementType.SMO_link #: Link.
    SYSTEM = SMO_elementType.SMO_sys #: System.
    POLLUTANT = SMO_elementType.SMO_pollut #: Pollutant.

class TimeAttribute(Enum):
    """
    Enumeration of the report time related attributes.

    :ivar REPORT_STEP: Report step size (seconds).
    :type REPORT_STEP: int
    :ivar NUM_PERIODS: Number of reporting periods.
    :type NUM_PERIODS: int
    """
    REPORT_STEP = SMO_time.SMO_reportStep #: Report step size (seconds).
    NUM_PERIODS = SMO_time.SMO_numPeriods #: Number of reporting periods.

class SubcatchAttribute(Enum):
    """
    Enumeration of the subcatchment attributes.

    :ivar RAINFALL: Subcatchment rainfall (in/hr or mm/hr).
    :type RAINFALL: int
    :ivar SNOW_DEPTH: Subcatchment snow depth (in or mm).
    :type SNOW_DEPTH: int
    :ivar EVAPORATION_LOSS: Subcatchment evaporation loss (in/hr or mm/hr).
    :type EVAPORATION_LOSS: int
    :ivar INFILTRATION_LOSS: Subcatchment infiltration loss (in/hr or mm/hr).
    :type INFILTRATION_LOSS: int
    :ivar RUNOFF_RATE: Subcatchment runoff flow (flow units).
    :type RUNOFF_RATE: int
    :ivar GROUNDWATER_OUTFLOW: Subcatchment groundwater flow (flow units).
    :type GROUNDWATER_OUTFLOW: int
    :ivar GW_TABLE: Subcatchment groundwater elevation (ft or m).
    :type GW_TABLE: int
    :ivar SOIL_MOISTURE: Subcatchment soil moisture content (-).
    :type SOIL_MOISTURE: int
    :ivar POLLUTANT_CONCENTRATION: Subcatchment pollutant concentration (-).
    :type POLLUTANT_CONCENTRATION: int
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
    Enumeration of the node attributes.

    :ivar INVERT_DEPTH: Node depth above invert (ft or m).
    :type INVERT_DEPTH: int
    :ivar HYDRAULIC_HEAD: Node hydraulic head (ft or m).
    :type HYDRAULIC_HEAD: int
    :ivar STORED_VOLUME: Node volume stored (ft3 or m3).
    :type STORED_VOLUME: int
    :ivar LATERAL_INFLOW: Node lateral inflow (flow units).
    :type LATERAL_INFLOW: int
    :ivar TOTAL_INFLOW: Node total inflow (flow units).
    :type TOTAL_INFLOW: int
    :ivar FLOODING_LOSSES: Node flooding losses (flow units).
    :type FLOODING_LOSSES: int
    :ivar POLLUTANT_CONCENTRATION: Node pollutant concentration (-).
    :type POLLUTANT_CONCENTRATION: int
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
    Enumeration of the link attributes.

    :ivar FLOW_RATE: Link flow rate (flow units).
    :type FLOW_RATE: int
    :ivar FLOW_DEPTH: Link flow depth (ft or m).
    :type FLOW_DEPTH: int
    :ivar FLOW_VELOCITY: Link flow velocity (ft/s or m/s).
    :type FLOW_VELOCITY: int
    :ivar FLOW_VOLUME: Link flow volume (ft3 or m3).
    :type FLOW_VOLUME: int
    :ivar CAPACITY: Link capacity (fraction of conduit filled).
    :type CAPACITY: int
    :ivar POLLUTANT_CONCENTRATION: Link pollutant concentration (-).
    :type POLLUTANT_CONCENTRATION: int
    """
    FLOW_RATE = SMO_linkAttribute.SMO_flow_rate_link #: Link flow rate (flow units).
    FLOW_DEPTH = SMO_linkAttribute.SMO_flow_depth #: Link flow depth (ft or m).
    FLOW_VELOCITY = SMO_linkAttribute.SMO_flow_velocity #: Link flow velocity (ft/s or m/s).
    FLOW_VOLUME = SMO_linkAttribute.SMO_flow_volume #: Link flow volume (ft3 or m3).
    CAPACITY = SMO_linkAttribute.SMO_capacity #: Link capacity (fraction of conduit filled).
    POLLUTANT_CONCENTRATION = SMO_linkAttribute.SMO_pollutant_conc_link #: Link pollutant concentration (-).

class SystemAttribute(Enum):
    """
    Enumeration of the system attributes.

    :ivar AIR_TEMP: Air temperature (deg. F or deg. C).
    :type AIR_TEMP: int
    :ivar RAINFALL: Rainfall intensity (in/hr or mm/hr).
    :type RAINFALL: int
    :ivar SNOW_DEPTH: Snow depth (in or mm).
    :type SNOW_DEPTH: int
    :ivar EVAP_INFIL_LOSS: Evaporation and infiltration loss rate (in/day or mm/day).
    :type EVAP_INFIL_LOSS: int
    :ivar RUNOFF_FLOW: Runoff flow (flow units).
    :type RUNOFF_FLOW: int
    :ivar DRY_WEATHER_INFLOW: Dry weather inflow (flow units).
    :type DRY_WEATHER_INFLOW: int
    :ivar GROUNDWATER_INFLOW: Groundwater inflow (flow units).
    :type GROUNDWATER_INFLOW: int
    :ivar RDII_INFLOW: Rainfall Derived Infiltration and Inflow (RDII) (flow units).
    :type RDII_INFLOW: int
    :ivar DIRECT_INFLOW: Direct inflow (flow units).
    :type DIRECT_INFLOW: int
    :ivar TOTAL_LATERAL_INFLOW: Total lateral inflow; sum of variables 4 to 8 (flow units).
    :type TOTAL_LATERAL_INFLOW: int
    :ivar FLOOD_LOSSES: Flooding losses (flow units).
    :type FLOOD_LOSSES: int
    :ivar OUTFALL_FLOWS: Outfall flow (flow units).
    :type OUTFALL_FLOWS: int
    :ivar VOLUME_STORED: Volume stored in storage nodes (ft3 or m3).
    :type VOLUME_STORED: int
    :ivar EVAPORATION_RATE: Evaporation rate (in/day or mm/day).
    :type EVAPORATION_RATE: int
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
    Exception class for SWMM output file processing errors.
    """
    def __init__(self, message: str) -> None:
        """
        Constructor to initialize the exception message.

        :param message: Error message.
        :type message: str
        """
        super().__init__(message)
        
cdef class Output:
    """
    Class to read and process the output file generated by the SWMM simulation.

    :cvar _output_file_handle: Handle to the SWMM output file.
    :cvar _version: Version of the SWMM output file.
    :cvar _units: Unit system used in the SWMM output file.
    :cvar _flow_units: Flow units used in the SWMM output file.
    :cvar _output_size: Size of the project in the SWMM output file.
    :cvar _pollutant_units: Pollutant units used in the SWMM output file.
    :cvar _start_date: Start date of the simulation in the SWMM output file.
    :cvar _report_step: Report step size in seconds.
    :cvar _num_periods: Number of reporting periods.
    :cvar _times: Times of the simulation in the SWMM output file.
    """
    cdef SMO_Handle _output_file_handle
    cdef int _version
    cdef int* _units
    cdef int _units_length
    cdef int _flow_units
    cdef int* _output_size
    cdef int _output_size_length
    cdef object _pollutant_units
    cdef object _start_date
    cdef int _report_step
    cdef int _num_periods
    cdef list _times

    def __cinit__(self, str output_file):
        """
        Constructor to open the SWMM output file.

        :param output_file: Path to the SWMM output file.
        :type output_file: str
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

        error_code = SMO_open(self._output_file_handle, c_output_file)

        # get error message if error code is not 0 and print it and prevent any memory leaks
        if error_code != 0:
            # create a buffer to store the error message
            error_message = self.check_error()

            if error_code > 400:
                self._output_file_handle = NULL

            if error_code == 434:
                raise FileNotFoundError(f"Error opening the SWMM output file {output_file}. Error code: {error_code}: {error_message}. The output file may be locked by another process.")
            else:
                raise SWMMOutputException(f"Error opening the SWMM output file {output_file}. Error code: {error_code}: {error_message}")

        # Read and cache output attributes for faster access 
        self._version = self.__get_version()
        self._units, self._units_length = self.__get_units() 
        self._flow_units = self.__get_flow_units()
        self._output_size, self._output_size_length = self.__get_output_size()
        self._pollutant_units = [ConcentrationUnits(i) for i in  self.__get_pollutant_units()]
        self._start_date = self.__get_start_date()
        self._report_step = self.get_time_attribute(TimeAttribute.REPORT_STEP)
        self._num_periods = self.get_time_attribute(TimeAttribute.NUM_PERIODS)
        self._times = [self._start_date + timedelta(seconds=self._report_step) * i for i in range(1, self._num_periods + 1)]
    
    def __enter__(self):
        """
        Method to return the SWMM output file instance.
        """
        return self

    def __close(self):
        """
        Method to close the SWMM output file instance.
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
        Method to close the SWMM output file instance.
        """
        self.__close()

    def __dealloc__(self):
        """
        Destructor to close the SWMM output file.
        """
        self.__close()

    @property
    def version(self) -> int:
        """
        Method to get the version of the SWMM output file.

        :return: Version of the SWMM output file.
        :rtype: str
        """
        return self._version

    cdef int __get_version(self):
        """
        Method to get the version of the SWMM output file.

        :return: Version of the SWMM output file.
        :rtype: str
        """
        cdef int error_code = 0
        cdef int version = 0

        error_code = SMO_getVersion(self._output_file_handle, &version)
        self.__validate_error_code(error_code)

        return version

    @property
    def output_size(self) -> Dict[str, int]:
        """
        Method to get the size of the project in the SWMM output file.

        :return: Size of the project in the SWMM output file.
        :rtype: int
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
        Method to get the size of the project in the SWMM output file.

        :return: Size of the project in the SWMM output file.
        :rtype: int
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
        Method to get the unit system used in the SWMM output file.

        :return: Tuple of the unit system, flow units, and pollutant units used in the SWMM output file.
        :rtype: Tuple[UnitSystem, FlowUnits, Optional[List[ConcentrationUnits]]]
        """
        return (
            UnitSystem(self._units[0]), 
            FlowUnits(self._units[1]), 
            [ConcentrationUnits(self._units[i]) for i in range(2, self._units_length)]
        )

    cdef (int*, int) __get_units(self):
        """
        Method to get the unit system used in the SWMM output file.

        :return: Tuple of comprised of an integer array of unit systems used and its length
        :rtype: Tuple[int*, int]
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
        Method to get the flow units used in the SWMM output file.

        :return: Flow units used in the SWMM output file.
        :rtype: FlowUnits
        """
        return FlowUnits(self._flow_units)

    cdef int __get_flow_units(self):
        """
        Method to get the flow units used in the SWMM output file.

        :return: Flow units used in the SWMM output file.
        :rtype: FlowUnits
        """
        cdef int error_code = 0
        cdef int flow_units = 0

        error_code = SMO_getFlowUnits(self._output_file_handle, &flow_units)
        self.__validate_error_code(error_code)

        return flow_units
     
    @property
    def pollutant_units(self) -> List[ConcentrationUnits]:
        """
        Method to get the pollutant units used in the SWMM output file.

        :return: Pollutant units used in the SWMM output file.
        :rtype: List[ConcentrationUnits]
        """
        return self._pollutant_units

    cdef list __get_pollutant_units(self):
        """
        Method to get the pollutant units used in the SWMM output file.

        :return: Pollutant units used in the SWMM output file.
        :rtype: List[ConcentrationUnits]
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

    @property
    def start_date(self) -> datetime:
        """
        Method to get the start date of the simulation in the SWMM output file.

        :return: Start date of the simulation in the SWMM output file.
        :rtype: datetime
        """
        return self._start_date

    cdef datetime __get_start_date(self):
        """
        Method to get the start date of the simulation in the SWMM output file.

        :return: Start date of the simulation in the SWMM output file.
        :rtype: datetime
        """
        cdef double swmm_datetime = 0

        error_code = SMO_getStartDate(self._output_file_handle, &swmm_datetime)
        self.__validate_error_code(error_code)

        return decode_swmm_datetime(swmm_datetime)

    @property
    def times(self) -> List[datetime]:
        """
        Method to get the times of the simulation in the SWMM output file.

        :return: Times of the simulation in the SWMM output file.
        :rtype: List[datetime]
        """
        return self._times

    def get_time_attribute(self, time_attribute: TimeAttribute) -> int:
        """
        Method to get the temporal attributes of the simulation in the SWMM output file.

        :return: Temporal attributes of the simulation in the SWMM output file.
        :rtype: int
        """
        cdef int error_code = 0
        cdef int temporal_attribute = -1

        error_code = SMO_getTimes(self._output_file_handle, <SMO_time>time_attribute.value, &temporal_attribute)
        self.__validate_error_code(error_code)

        return temporal_attribute

    def get_element_name(self, element_type: ElementType,  element_index: int) -> str:
        """
        Method to get the name of an element in the SWMM output file.

        :param element_type: Type of the element.
        :type element_type: int
        :param index: Index of the element.
        :type index: int
        :return: Name of the element.
        :rtype: str
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
        Method to get the names of all elements of a given type in the SWMM output file.

        :param element_type: Type of the element.
        :type element_type: int
        :return: Names of all elements of the given type.
        :rtype: List[str]
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
    
    def get_subcatchment_timeseries(self, element_index: int, attribute: SubcatchAttribute, start_date_index: int = 0, end_date_index: int = -1) -> Dict[datetime, float]:
        """
        Method to get the time series data for a subcatchment attribute in the SWMM output file.

        :param subcatchment_index: Index of the subcatchment.
        :type subcatchment_index: int
        :param attribute: Subcatchment attribute.
        :type attribute: SubcatchAttribute
        :param start_date_index: Start date index. Default is 0.
        :type start_date_index: int
        :param end_date_index: End date index. Default is the last date index.
        :type end_date_index: int        
        :return: Time series data for the subcatchment attribute.
        :rtype: Dict[datetime, double]
        TODO: Add option to return memoryview
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getSubcatchSeries(
            self._output_file_handle, 
            element_index, 
            <SMO_subcatchAttribute>attribute.value, 
            start_date_index, 
            end_date_index, 
            &values, &length
        )
        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_node_timeseries(self, element_index: int, attribute: NodeAttribute, start_date_index: int = 0, end_date_index: int = -1) -> Dict[datetime, float]:
        """
        Method to get the time series data for a node attribute in the SWMM output file.

        :param node_index: Index of the node.
        :type node_index: int
        :param attribute: Node attribute.
        :type attribute: NodeAttribute
        :param start_date_index: Start date index. Default is 0.
        :type start_date_index: int
        :param end_date_index: End date index. Default is the last date index.
        :type end_date_index: int
        :return: Time series data for the node attribute.
        :rtype: Dict[datetime, double]
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        if end_date_index == -1:
            end_date_index = self._num_periods
        
        error_code = SMO_getNodeSeries(
            self._output_file_handle, 
            element_index, 
            <SMO_nodeAttribute>attribute.value, 
            start_date_index, 
            end_date_index, 
            &values, &length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_link_timeseries(self, element_index: int, attribute: LinkAttribute, start_date_index: int = 0, end_date_index: int = -1) -> Dict[datetime, float]:
        """
        Method to get the time series data for a link attribute in the SWMM output file.

        :param link_index: Index of the link.
        :type link_index: int
        :param attribute: Link attribute.
        :type attribute: LinkAttribute
        :param start_date_index: Start date index. Default is 0.
        :type start_date_index: int
        :param end_date_index: End date index. Default is the last date index.
        :type end_date_index: int
        :return: Time series data for the link attribute.
        :rtype: Dict[datetime, double]
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        if end_date_index == -1:
            end_date_index = self._num_periods
        
        error_code = SMO_getLinkSeries(
            self._output_file_handle, 
            element_index, 
            <SMO_linkAttribute>attribute.value, 
            start_date_index, 
            end_date_index, 
            &values, &length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results
    
    def get_system_timeseries(self, attribute: SystemAttribute, start_date_index: int = 0, end_date_index: int = -1) -> Dict[datetime, float]:
        """
        Method to get the time series data for a system attribute in the SWMM output file.

        :param attribute: System attribute.
        :type attribute: SystemAttribute
        :param start_date_index: Start date index. Default is 0.
        :type start_date_index: int
        :param end_date_index: End date index. Default is the last date index.
        :type end_date_index: int
        :return: Time series data for the system attribute.
        :rtype: Dict[datetime, double]
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        if end_date_index == -1:
            end_date_index = self._num_periods

        error_code = SMO_getSystemSeries(
            self._output_file_handle, 
            <SMO_systemAttribute>attribute.value, 
            start_date_index, 
            end_date_index, 
            &values, &length
        )

        self.__validate_error_code(error_code)

        results = dict(zip(self._times[start_date_index:end_date_index], <float[:length]>values))

        if values != NULL:
            free(values)

        return results

    def get_subcatchment_values_by_time_and_attribute(self, time_index: int, attribute: SubcatchAttribute) -> Dict[str, float]:
        """
        Method to get the subcatchment values for all subcatchments for a given time index and attribute.

        :param time_index: Time index.
        :type time_index: int
        :param attribute: Subcatchment attribute.
        :type attribute: SubcatchAttribute
        :return: Subcatchment values for all subcatchments. 
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        error_code = SMO_getSubcatchAttribute(
            self._output_file_handle, 
            time_index, 
            <SMO_subcatchAttribute>attribute.value, 
            &values, 
            &length
        )
        
        self.__validate_error_code(error_code)

        subcatchment_values = dict(zip(self.get_element_names(ElementType.SUBCATCHMENT), <float[:length]>values))

        if values != NULL:
            free(values)
        
        return subcatchment_values

    def get_node_values_by_time_and_attribute(self, time_index: int, attribute: NodeAttribute) -> Dict[str, float]:
        """
        Method to get the node values for all nodes for a given time index and attribute.

        :param time_index: Time index.
        :type time_index: int
        :param attribute: Node attribute.
        :type attribute: NodeAttribute
        :return: Node values for all nodes.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        error_code = SMO_getNodeAttribute(
            self._output_file_handle, 
            time_index, 
            <SMO_nodeAttribute>attribute.value, 
            &values,
            &length
        )

        self.__validate_error_code(error_code)

        node_values = dict(zip(self.get_element_names(ElementType.NODE), <float[:length]>values))

        if values != NULL:
            free(values)
        
        return node_values

    def get_link_values_by_time_and_attribute(self, time_index: int, attribute: LinkAttribute) -> Dict[str, float]:
        """
        Method to get the link values for all links for a given time index and attribute.

        :param time_index: Time index.
        :type time_index: int
        :param attribute: Link attribute.
        :type attribute: LinkAttribute
        :return: Link values for all links.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        error_code = SMO_getLinkAttribute(
            self._output_file_handle, 
            time_index, 
            <SMO_linkAttribute>attribute.value, 
            &values,
            &length
        )
        
        self.__validate_error_code(error_code)

        link_values = dict(zip(self.get_element_names(ElementType.LINK), <float[:length]>values))

        if values != NULL:
            free(values)
        
        return link_values
   
    def get_system_values_by_time_and_attribute(self, time_index: int, attribute: SystemAttribute) -> Dict[str, float]:
        """
        Method to get the system values for a given time index and attribute.
        
        :param time_index: Time index.
        :type time_index: int
        :param attribute: System attribute.
        :type attribute: SystemAttribute
        :return: System values.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0

        error_code = SMO_getSystemAttribute(
            self._output_file_handle, 
            time_index, 
            <SMO_systemAttribute>attribute.value, 
            &values,
            &length
        )
        
        self.__validate_error_code(error_code)

        system_values = dict(zip([SystemAttribute(attribute).name], <float[:length]>values))


        if values != NULL:
            free(values)
        
        return system_values

    def get_subcatchment_values_by_time_and_element_index(self, time_index: int, element_index: int) -> Dict[str, float]:
        """
        Method to get all attributes of a given subcatchment for specified time.

        :param time_index: Time index.
        :type time_index: int
        :param subcatchment_index: Index of the subcatchment.
        :type subcatchment_index: int
        :return: Dictionary of subcatchment attributes.
        :rtype: Dict[str, float]
        """
        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(SubcatchAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []

        error_code = SMO_getSubcatchResult(
            self._output_file_handle, 
            time_index, 
            element_index, 
            &values, &length
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

    def get_node_values_by_time_and_element_index(self, time_index: int, element_index: int) -> Dict[str, float]:
        """
        Method to get all attributes of a given node for specified time.

        :param time_index: Time index.
        :type time_index: int
        :param node_index: Index of the node.
        :type node_index: int
        :return: Dictionary of node attributes.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(NodeAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []

        error_code = SMO_getNodeResult(
            self._output_file_handle, 
            time_index, 
            element_index, 
            &values, &length
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

    def get_link_values_by_time_and_element_index(self, time_index: int, element_index: int) -> Dict[str, float]:
        """
        Method to get all attributes of a given link for specified time.

        :param time_index: Time index.
        :type time_index: int
        :param link_index: Index of the link.
        :type link_index: int

        :return: Dictionary of link attributes.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(LinkAttribute) - 1
        cdef list pollutant_names = self.get_element_names(ElementType.POLLUTANT)
        cdef list attrib_names = []

        error_code = SMO_getLinkResult(
            self._output_file_handle, 
            time_index, 
            element_index, 
            &values, &length
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
        Method to get all attributes of the system for specified time.

        :param time_index: Time index.
        :type time_index: int

        :return: Dictionary of system attributes.
        :rtype: Dict[str, float]
        """

        cdef int error_code = 0
        cdef float* values = NULL
        cdef int length = 0
        cdef int enum_values_length = len(SystemAttribute) - 1

        error_code = SMO_getSystemResult(
            self._output_file_handle, 
            time_index, 
            0,
            &values, &length
        )

        self.__validate_error_code(error_code)

        system_values = dict(zip([SystemAttribute(i).name for i in range(enum_values_length)], <float[:length]>values))

        if values != NULL:
            free(values)

        return system_values

    cdef str check_error(self):
        """
        Method to check if there is an error in the SWMM output file instance reader.

        :return: Error message if there is an error, otherwise None.
        :rtype: str
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
            return ""

    cdef str __validate_error_code(self, int error_code):
        """
        Method to validate the error code and return the error message if there is an error.

        :param error_code: Error code to validate.
        :type error_code: int
        :return: Error message if there is an error, otherwise None.
        :rtype: str
        """

        if error_code != 0:
            error_message = self.check_error()
            raise SWMMOutputException(f"Error code: {error_code}: {error_message}")
        else:
            return ""