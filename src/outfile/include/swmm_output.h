/*!
* \file swmm_output.h
* \author Colleen Barr (US EPA - ORD/NHEERL)
* \author Michael Tryby (US EPA) (Modified)
* \author Bryant McDonnell (Modified)
* \brief Header file for SWMM output API.
* \date Created: 2017-08-25
* \date Last edited: 2024-10-17
*/

#ifndef SWMM_OUTPUT_H_
#define SWMM_OUTPUT_H_

/*! 
* \def MAXFILENAME
* \brief Maximum number of characters in a file path 
*/
#define MAXFILENAME 259

/*!
* \def MAXELENAME
* \brief Maximum number of characters in a element name
*/
#define MAXELENAME 31

/*! 
* \typedef SMO_Handle 
* \brief Opaque pointer to struct. Do not access variables. 
*/
typedef void *SMO_Handle;

#include "swmm_output_export.h"
#include "swmm_output_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
* \brief Initializes the SWMM output file handle
* \param[out] p_handle Opaque pointer to SWMM output file handle
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_SWMM_OUTPUT_API SMO_init(SMO_Handle *p_handle);

/*!
* \brief Closes the SWMM output file handle
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_SWMM_OUTPUT_API SMO_close(SMO_Handle *p_handle);

/*!
* \brief Opens a SWMM output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] path Path to the SWMM output file
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_SWMM_OUTPUT_API SMO_open(SMO_Handle p_handle, const char *path);

/*!
* \brief Retrieves the model version number that created the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] version Pointer to the version number
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_SWMM_OUTPUT_API SMO_getVersion(SMO_Handle p_handle, int *version);

/*!
* \brief Get project size.
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] elementCount Array of element counts
* \param[out] length Length of elementCount array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getProjectSize(SMO_Handle p_handle, int **elementCount, int *length);

/*!
* \brief Retrieves the unit system used in the SWMM model
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] unitSystem Pointer to the unit system
* \param[out] length Length of unitFlag array
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getUnits(SMO_Handle p_handle, int **unitFlag, int *length);

/*!
* \brief Returns unit flag for flow
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] unitFlag Flow unit flag
*  0: CFS (cubic feet per second), 
*  1: GPM (gallons per minute), 
*  2: MGD (million gallons per day), 
*  3: CMS (cubic meters per second), 
*  4: LPS (liters per second), 
*  5: MLD (million liters per day)
* \return Error code 
*/
int EXPORT_SWMM_OUTPUT_API SMO_getFlowUnits(SMO_Handle p_handle, int *unitFlag);

/*!
* \brief Returns unit flag for pollutant. Concentration units are located after the pollutant ID
* names and before the object properties start, and are stored for each pollutant. They're stored
* as 4-byte integers with the following codes:
* 0: mg/L
* 1: ug/L
* 2: count/L
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] unitFlag Array of unit flags
* \param[out] length Length of unitFlag array
* \returns Error code
* \note Valid values are 0 to Npolluts-1
*/
int EXPORT_SWMM_OUTPUT_API SMO_getPollutantUnits(SMO_Handle p_handle, int **unitFlag, int *length);

/*!
* \brief Retrieves the start date of the simulation
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[out] date Pointer to the start date
* \return Error code 0 on success, -1 on failure
*/
int EXPORT_SWMM_OUTPUT_API SMO_getStartDate(SMO_Handle p_handle, double *date);

/*!
* \brief Retrieves the number of reporting periods in the simulation
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] code The type of reporting attribute to retrieve
* \param[out] time Pointer to the reporting attribute value
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getTimes(SMO_Handle p_handle, SMO_time code, int *time);

/*!
* \brief Retrieves the element name by index and type from the SWMM output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[in] elementIndex The index of the element
* \param[out] elementName Pointer to the element name
* \param[out] length Pointer to the size of the elementName array
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getElementName(
    SMO_Handle p_handle, 
    SMO_elementType type, 
    int elementIndex, 
    char **elementName, 
    int *size
);


/*!
* \brief Retrieves the number of variables for a given element type that are stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[out] count Pointer to the number of variables
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getNumVars(
    SMO_Handle p_handle,
    SMO_elementType type, 
    int *count
);

/*!
* \brief Retrieves the variable code for a given element type and variable index that is stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[in] varIndex The index of the variable
* \param[out] varCode Pointer to the variable code
* \return Variable code or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getVarCode(SMO_Handle p_handle, SMO_elementType type, int varIndex, int *varCode);

/*!
* \brief Retrieves the variable codes for a given element type that are stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[out] varCodes Pointer to the variable codes
* \param[out] size Pointer to the size of the varCodes array
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getVarCodes(SMO_Handle p_handle, SMO_elementType type, int **varCodes, int *size);

/*!
* \brief Retrieves the number of properties for a given element type that are stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[out] count Pointer to the number of properties
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getNumProperties(SMO_Handle p_handle, SMO_elementType type, int *count);

/*!
* \brief Retrieves the property code for a given element type and property index that is stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[in] propertyIndex The index of the property
* \param[out] propertyCode Pointer to the property code
* \return Property code or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getPropertyCode(SMO_Handle p_handle, SMO_elementType type, int propertyIndex, int *propertyCode);

/*!
* \brief Retrieves the property codes for a given element type that are stored in the output file
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[out] propertyCodes Pointer to the property codes
* \param[out] size Pointer to the size of the propertyCodes array
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getPropertyCodes(SMO_Handle p_handle, SMO_elementType type, int **propertyCodes, int *size);

/*!
* \brief Retrieves the value of a property for a given element type, property index, and element index
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[in] propertyIndex The index of the property
* \param[in] elementIndex The index of the element
* \param[out] value Pointer to the property value
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getPropertyValue(
    SMO_Handle p_handle, 
    SMO_elementType type, 
    int propertyIndex, 
    int elementIndex, 
    float *value
);

/*!
* \brief Retrieves all property values for a given element type and element index
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] type The type of element
* \param[in] elementIndex The index of the element
* \param[out] outValueArray Pointer to the property values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getPropertyValues(
    SMO_Handle p_handle, 
    SMO_elementType type, 
    int elementIndex, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves subcatchment attribute values for a given time period and attribute type 
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] subcatchIndex The index of the subcatchment
* \param[in] attr The subcatchment attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from 
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the subcatchment attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSubcatchSeries(
    SMO_Handle p_handle, 
    int subcatchIndex, 
    SMO_subcatchAttribute attr, 
    int startPeriod, 
    int endPeriod, 
    float **outValueArray,
    int *length
);

/*!
* \brief Retrieves node attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] nodeIndex The index of the node
* \param[in] attr The node attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the node attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getNodeSeries(
    SMO_Handle p_handle, 
    int nodeIndex, 
    SMO_nodeAttribute attr, 
    int startPeriod, 
    int endPeriod, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves link attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] linkIndex The index of the link
* \param[in] attr The link attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the link attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getLinkSeries(
    SMO_Handle p_handle, 
    int linkIndex, 
    SMO_linkAttribute attr, 
    int startPeriod, 
    int endPeriod, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves system attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] attr The system attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from 
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the system attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSystemSeries(
    SMO_Handle p_handle, 
    SMO_systemAttribute attr, 
    int startPeriod, 
    int endPeriod, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves subcatchment attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The subcatchment attribute type to retrieve
* \param[out] outValueArray Pointer to the subcatchment attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSubcatchAttribute(
    SMO_Handle p_handle, 
    int timeIndex, 
    SMO_subcatchAttribute attr, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves node attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The node attribute type to retrieve
* \param[out] outValueArray Pointer to the node attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getNodeAttribute(
    SMO_Handle p_handle, 
    int timeIndex, 
    SMO_nodeAttribute attr, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves link attribute values for a given time period and attribute type 
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The link attribute type to retrieve
* \param[out] outValueArray Pointer to the link attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getLinkAttribute(
    SMO_Handle p_handle, 
    int timeIndex, 
    SMO_linkAttribute attr, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves system attribute values for a given time period and attribute type
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The system attribute type to retrieve
* \param[out] outValueArray Pointer to the system attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSystemAttribute(
    SMO_Handle p_handle, 
    int timeIndex, 
    SMO_systemAttribute attr, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves subcatchment result values for a given time period
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] subcatchIndex The index of the subcatchment
* \param[out] outValueArray Pointer to the subcatchment result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSubcatchResult(
    SMO_Handle p_handle,
    int timeIndex, 
    int subcatchIndex,
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves node result values for a given time period
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] nodeIndex The index of the node
* \param[out] outValueArray Pointer to the node result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getNodeResult(
    SMO_Handle p_handle, 
    int timeIndex, 
    int nodeIndex, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves link result values for a given time period
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] linkIndex The index of the link
* \param[out] outValueArray Pointer to the link result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getLinkResult(
    SMO_Handle p_handle, 
    int timeIndex, 
    int linkIndex, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Retrieves system result values for a given time period
* \param[in] p_handle Opaque pointer to SWMM output file handle
* \param[in] timeIndex The index of the time period
* \param[in] dummyIndex The index of the system
* \param[out] outValueArray Pointer to the system result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_getSystemResult(
    SMO_Handle p_handle, 
    int timeIndex, 
    int dummyIndex, 
    float **outValueArray, 
    int *length
);

/*!
* \brief Frees memory allocated by the API for the outValueArray
* \param[in] array Pointer to the outValueArray
*/
void EXPORT_SWMM_OUTPUT_API SMO_free(void **array);

/*!
* \brief Clears the error status of the SMO_Handle
* \param[in] p_handle Opaque pointer to SWMM output file handle
*/
void EXPORT_SWMM_OUTPUT_API SMO_clearError(SMO_Handle p_handle_in);

/*!
* \brief Checks for error in the error handle and copies the error message to the
* message buffer 
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] msg_buffer Error message buffer 
* \return Error code
*/
int EXPORT_SWMM_OUTPUT_API SMO_checkError(SMO_Handle p_handle_in, char **msg_buffer);

#ifdef __cplusplus
}
#endif

#endif /* SWMM_OUTPUT_H_ */
