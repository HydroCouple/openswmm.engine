# Description: Cython module for openswmmcore output file processing and data extraction functions for the openswmmcore python package.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# cython: language_level=3
# SWMM datetime encode decoder functions (not very elegant/need to fix later)
# cdef extern from "datetime.h"

# SWMM output enumeration types.
cdef extern from "openswmm_output_enums.h":

    # Unit system used in the output file
    ctypedef enum SMO_unitSystem:
        SMO_US # US customary units
        SMO_SI # SI metric units

    # Flow units used in the simulation
    ctypedef enum SMO_flowUnits:
        SMO_CFS  # Cubic feet per second
        SMO_GPM  # Gallons per minute
        SMO_MGD  # Million gallons per day
        SMO_CMS  # Cubic meters per second
        SMO_LPS  # Liters per second
        SMO_MLD  # Million liters per day

    # Concentration units used in the simulation
    ctypedef enum SMO_concUnits:
        SMO_MG    # Milligrams per liter
        SMO_UG    # Micrograms per liter
        SMO_COUNT # Counts per liter
        SMO_NONE  # No units

    # SWMM element types
    ctypedef enum SMO_elementType:
        SMO_subcatch  # Subcatchment
        SMO_node      # Node
        SMO_link      # Link
        SMO_sys       # System
        SMO_pollut    # Pollutant

    # Report time related attributes
    ctypedef enum SMO_time:
        SMO_reportStep  # Report step size (seconds)
        SMO_numPeriods  # Number of reporting periods

    # Subcatchment attributes
    ctypedef enum SMO_subcatchAttribute:
        SMO_rainfall_subcatch     # Subcatchment rainfall (in/hr or mm/hr)
        SMO_snow_depth_subcatch   # Subcatchment snow depth (in or mm)
        SMO_evap_loss             # Subcatchment evaporation loss (in/hr or mm/hr)
        SMO_infil_loss            # Subcatchment infiltration loss (in/hr or mm/hr)
        SMO_runoff_rate           # Subcatchment runoff flow (flow units)
        SMO_gwoutflow_rate        # Subcatchment groundwater flow (flow units)
        SMO_gwtable_elev          # Subcatchment groundwater elevation (ft or m)
        SMO_soil_moisture         # Subcatchment soil moisture content (-)
        SMO_pollutant_conc_subcatch  # Subcatchment pollutant concentration (-)

    # Node attributes
    ctypedef enum SMO_nodeAttribute:
        SMO_invert_depth          # Node depth above invert (ft or m)
        SMO_hydraulic_head        # Node hydraulic head (ft or m)
        SMO_stored_ponded_volume  # Node volume stored (ft3 or m3)
        SMO_lateral_inflow        # Node lateral inflow (flow units)
        SMO_total_inflow          # Node total inflow (flow units)
        SMO_flooding_losses       # Node flooding losses (flow units)
        SMO_pollutant_conc_node   # Node pollutant concentration (-)

    # Link attributes
    ctypedef enum SMO_linkAttribute:
        SMO_flow_rate_link        # Link flow rate (flow units)
        SMO_flow_depth            # Link flow depth (ft or m)
        SMO_flow_velocity         # Link flow velocity (ft/s or m/s)
        SMO_flow_volume           # Link flow volume (ft3 or m3)
        SMO_capacity              # Link capacity (fraction of conduit filled)
        SMO_pollutant_conc_link   # Link pollutant concentration (-)

    # System attributes
    ctypedef enum SMO_systemAttribute:
        SMO_air_temp              # Air temperature (deg. F or deg. C)
        SMO_rainfall_system       # Rainfall intensity (in/hr or mm/hr)
        SMO_snow_depth_system     # Snow depth (in or mm)
        SMO_evap_infil_loss       # Evaporation and infiltration loss rate (in/day or mm/day)
        SMO_runoff_flow           # Runoff flow (flow units)
        SMO_dry_weather_inflow    # Dry weather inflow (flow units)
        SMO_groundwater_inflow    # Groundwater inflow (flow units)
        SMO_RDII_inflow           # Rainfall Derived Infiltration and Inflow (RDII) (flow units)
        SMO_direct_inflow         # Direct inflow (flow units)
        SMO_total_lateral_inflow  # Total lateral inflow; sum of variables 4 to 8 (flow units)
        SMO_flood_losses          # Flooding losses (flow units)
        SMO_outfall_flows         # Outfall flow (flow units)
        SMO_volume_stored         # Volume stored in storage nodes (ft3 or m3)
        SMO_evap_rate             # Evaporation rate (in/day or mm/day)


# SWMM Output API functions
cdef extern from "openswmm_output.h":

    # Opaque pointer to struct. Do not access variables.
    ctypedef void* SMO_Handle

    # Maximum length of a file name
    cdef const int MAXFILENAME

    # Maximum length of an element name
    cdef const int MAXELENAME
    
    # Initializes the SWMM output file handle
    # p_handle: Pointer to a SMO_Handle
    # Returns: Error code 0 if successful or -1 if an error occurs
    int SMO_init(SMO_Handle *p_handle)

    # Closes the SWMM output file handle
    # p_handle: Pointer to a SMO_Handle
    # Returns: Error code 0 if successful or -1 if an error occurs
    int SMO_close(SMO_Handle *p_handle)

    # Opens a SWMM output file
    # p_handle: Pointer to a SMO_Handle
    # path: Path to the SWMM output file
    # Returns: Error code
    int SMO_open(SMO_Handle p_handle, const char *path)

    # Retrieves the model version number that created the output file
    # p_handle: Pointer to a SMO_Handle
    # version: Pointer to the version number
    # Returns: Error code
    int SMO_getVersion(SMO_Handle p_handle, int *version)

    # Retrieves the number of elements in the SWMM model
    # p_handle: Pointer to a SMO_Handle
    # elementCount: Pointer to the number of elements
    # length: Pointer to the length of the elementCount array
    # Returns: Error code
    int SMO_getProjectSize(SMO_Handle p_handle, int **elementCount, int *length)

    # Retrieves the unit system used in the SWMM model
    # p_handle: Pointer to a SMO_Handle
    # unitSystem: Pointer to the unit system
    # Returns: Error code
    int SMO_getUnits(SMO_Handle p_handle, int **unitFlag, int *length)

    # Retrieves the flow units used in the SWMM model
    # p_handle: Pointer to a SMO_Handle
    # unitFlag: Pointer to the flow units
    # Returns: Error code
    int SMO_getFlowUnits(SMO_Handle p_handle, int *unitFlag)

    # Retrieves the pollutant units used in the SWMM model
    # p_handle: Pointer to a SMO_Handle
    # unitFlag: Pointer to the pollutant units
    # length: Pointer to the length of the unitFlag array
    # Returns: Error code
    int SMO_getPollutantUnits(SMO_Handle p_handle, int **unitFlag, int *length)

    # Retrieves the start date of the simulation
    # p_handle: Pointer to a SMO_Handle
    # date: Pointer to the start date
    # Returns: Error code
    int SMO_getStartDate(SMO_Handle p_handle, double *date)

    # Retrieves the number of reporting periods in the simulation
    # p_handle: Pointer to a SMO_Handle
    # code: The type of reporting attribute to retrieve
    # time: Pointer to the reporting attribute value
    # Returns: Error code
    int SMO_getTimes(SMO_Handle p_handle, int code, int *time)

    # Retrieves the element name
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # elementIndex: The index of the element
    # elementName: Pointer to the element name
    # size: Pointer to the size of the elementName array
    # Returns: Error code
    int SMO_getElementName(SMO_Handle p_handle, int type, int elementIndex, char **elementName, int *size)

    # Retrieves the number of attributes for a given element type
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # count: Pointer to the number of attributes
    # Returns: Error code
    int SMO_getNumVars(SMO_Handle p_handle, SMO_elementType type, int *count)


    # Retrieves the attribute code for a given element type and attribute index
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # varIndex: The index of the attribute
    # Returns: The attribute code
    int SMO_getVarCode(SMO_Handle p_handle, SMO_elementType type, int varIndex, int *varCode)

    # Retrieves the attribute codes for a given element type
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # varCodes: Pointer to the attribute codes
    # size: Pointer to the size of the varCodes array
    # Returns: Error code
    int SMO_getVarCodes(SMO_Handle p_handle, SMO_elementType type, int **varCodes, int *size)
    
    # Retrieves the number of properties for a given element type
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # count: Pointer to the number of properties
    # Returns: Error code
    int SMO_getNumProperties(SMO_Handle p_handle, SMO_elementType type, int *count)

    # Retrieves the property code for a given element type and property index
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # propertyIndex: The index of the property
    # propertyCode: Pointer to the property code
    # Returns: The property code
    int SMO_getPropertyCode(SMO_Handle p_handle, SMO_elementType type, int propertyIndex, int *propertyCode)

    # Retrieves the property codes for a given element type
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # propertyCodes: Pointer to the property codes
    # size: Pointer to the size of the propertyCodes array
    # Returns: Error code
    int SMO_getPropertyCodes(SMO_Handle p_handle, SMO_elementType type, int **propertyCodes, int *size)

    # Retrieves the property value for a given element type, property index, and element index
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # propertyIndex: The index of the property
    # elementIndex: The index of the element
    # value: Pointer to the property value
    int SMO_getPropertyValue(SMO_Handle p_handle, SMO_elementType type, int propertyIndex, int elementIndex, float *value)

    # Retrieves the property values for a given element type, property index, and element index
    # p_handle: Pointer to a SMO_Handle
    # type: The type of element
    # propertyIndex: The index of the property
    # elementIndex: The index of the element
    # outValueArray: Pointer to the property values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getPropertyValues(SMO_Handle p_handle, SMO_elementType type, int elementIndex, float **outValueArray, int *length)

    # Retrieves subcatchment attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # subcatchIndex: The index of the subcatchment
    # attr: The subcatchment attribute type to retrieve
    # startPeriod: The starting time period to retrieve data from
    # endPeriod: The ending time period to retrieve data from
    # outValueArray: Pointer to the subcatchment attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSubcatchSeries(SMO_Handle p_handle, int subcatchIndex, SMO_subcatchAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length)

    # Retrieves node attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # nodeIndex: The index of the node
    # attr: The node attribute type to retrieve
    # startPeriod: The starting time period to retrieve data from
    # endPeriod: The ending time period to retrieve data from
    # outValueArray: Pointer to the node attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getNodeSeries(SMO_Handle p_handle, int nodeIndex, SMO_nodeAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length)

    # Retrieves link attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # linkIndex: The index of the link
    # attr: The link attribute type to retrieve
    # startPeriod: The starting time period to retrieve data from
    # endPeriod: The ending time period to retrieve data from
    # outValueArray: Pointer to the link attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getLinkSeries(SMO_Handle p_handle, int linkIndex, SMO_linkAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length)

    # Retrieves system attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # attr: The system attribute type to retrieve
    # startPeriod: The starting time period to retrieve data from
    # endPeriod: The ending time period to retrieve data from
    # outValueArray: Pointer to the system attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSystemSeries(SMO_Handle p_handle, SMO_systemAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length)

    # Retrieves subcatchment attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # attr: The subcatchment attribute type to retrieve
    # outValueArray: Pointer to the subcatchment attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSubcatchAttribute(SMO_Handle p_handle, int timeIndex, SMO_subcatchAttribute attr, float **outValueArray, int *length)

    # Retrieves node attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # attr: The node attribute type to retrieve
    # outValueArray: Pointer to the node attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getNodeAttribute(SMO_Handle p_handle, int timeIndex, SMO_nodeAttribute attr, float **outValueArray, int *length)

    # Retrieves link attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # attr: The link attribute type to retrieve
    # outValueArray: Pointer to the link attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getLinkAttribute(SMO_Handle p_handle, int timeIndex, SMO_linkAttribute attr, float **outValueArray, int *length)

    # Retrieves system attribute values for a given time period and attribute type
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # attr: The system attribute type to retrieve
    # outValueArray: Pointer to the system attribute values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSystemAttribute(SMO_Handle p_handle, int timeIndex, SMO_systemAttribute attr, float **outValueArray, int *length)

    # Retrieves subcatchment result values for a given time period
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # subcatchIndex: The index of the subcatchment
    # outValueArray: Pointer to the subcatchment result values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSubcatchResult(SMO_Handle p_handle, int timeIndex, int subcatchIndex, float **outValueArray, int *length)

    # Retrieves node result values for a given time period
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # nodeIndex: The index of the node
    # outValueArray: Pointer to the node result values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getNodeResult(SMO_Handle p_handle, int timeIndex, int nodeIndex, float **outValueArray, int *length)

    # Retrieves link result values for a given time period
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # linkIndex: The index of the link
    # outValueArray: Pointer to the link result values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getLinkResult(SMO_Handle p_handle, int timeIndex, int linkIndex, float **outValueArray, int *length)

    # Retrieves system result values for a given time period
    # p_handle: Pointer to a SMO_Handle
    # timeIndex: The index of the time period
    # dummyIndex: The index of the system
    # outValueArray: Pointer to the system result values
    # length: Pointer to the length of the outValueArray array
    # Returns: Error code
    int SMO_getSystemResult(SMO_Handle p_handle, int timeIndex, int dummyIndex, float **outValueArray, int *length)

    # Frees memory allocated by the API for the outValueArray
    # array: Pointer to the outValueArray
    void SMO_free(void **array)

    # Clears the error status of the SMO_Handle
    # p_handle: Pointer to a SMO_Handle
    void SMO_clearError(SMO_Handle p_handle)

    # Retrieves the error message from the SMO_Handle
    # p_handle: Pointer to a SMO_Handle
    # msg_buffer: Pointer to the error message
    # Returns: Error code
    int SMO_checkError(SMO_Handle p_handle, char **msg_buffer)