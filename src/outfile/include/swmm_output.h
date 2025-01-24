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


#include "swmm_output_enums.h"
#include "swmm_output_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
* \brief Initializes the SWMM output file handle
* \param[out] p_handle Pointer to a SMO_Handle
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_OUT_API SMO_init(SMO_Handle *p_handle);

/*!
* \brief Closes the SWMM output file handle
* \param[in] p_handle Pointer to a SMO_Handle
* \return Error code 0 if successful or -1 if an error occurs
*/
int EXPORT_OUT_API SMO_close(SMO_Handle *p_handle);

/*!
* \brief Opens a SWMM output file
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] path Path to the SWMM output file
* \return Error code
*/
int EXPORT_OUT_API SMO_open(SMO_Handle p_handle, const char *path);

/*!
* \brief Retrieves the model version number that created the output file
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] version Pointer to the version number
* \return Error code
*/
int EXPORT_OUT_API SMO_getVersion(SMO_Handle p_handle, int *version);

/*!
* \brief Retrieves the number of elements in the SWMM model
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] elementCount Pointer to the number of elements
* \param[out] length Pointer to the length of the elementCount array
* \return Error code
*/
int EXPORT_OUT_API SMO_getProjectSize(SMO_Handle p_handle, int **elementCount, int *length);

/*!
* \brief Retrieves the unit system used in the SWMM model
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] unitSystem Pointer to the unit system
* \return Error code
*/
int EXPORT_OUT_API SMO_getUnits(SMO_Handle p_handle, int **unitFlag, int *length);

/*!
* \brief Retrieves the flow units used in the SWMM model
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] unitFlag Pointer to the flow units
* \return Error code
*/
int EXPORT_OUT_API SMO_getFlowUnits(SMO_Handle p_handle, int *unitFlag);

/*!
* \brief Retrieves the pollutant units used in the SWMM model
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] unitFlag Pointer to the pollutant units
* \param[out] length Pointer to the length of the unitFlag array
* \return Error code
*/
int EXPORT_OUT_API SMO_getPollutantUnits(SMO_Handle p_handle, int **unitFlag, int *length);

/*!
* \brief Retrieves the start date of the simulation
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] date Pointer to the start date
* \return Error code
*/
int EXPORT_OUT_API SMO_getStartDate(SMO_Handle p_handle, double *date);

/*!
* \brief Retrieves the number of reporting periods in the simulation
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] code The type of reporting attribute to retrieve
* \param[out] time Pointer to the reporting attribute value
* \return Error code
*/
int EXPORT_OUT_API SMO_getTimes(SMO_Handle p_handle, SMO_time code, int *time);

/*!
* \brief Retrieves the element name
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] type The type of element
* \param[in] elementIndex The index of the element
* \param[out] elementName Pointer to the element name
* \param[out] size Pointer to the size of the elementName array
* \return Error code
*/
int EXPORT_OUT_API SMO_getElementName(SMO_Handle p_handle, SMO_elementType type, int elementIndex, char **elementName, int *size);

/*!
* \brief Retrieves subcatchment attribute values for a given time period and attribute type 
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] subcatchIndex The index of the subcatchment
* \param[in] attr The subcatchment attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from 
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the subcatchment attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSubcatchSeries(SMO_Handle p_handle, int subcatchIndex, SMO_subcatchAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length);

/*!
* \brief Retrieves node attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] nodeIndex The index of the node
* \param[in] attr The node attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the node attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getNodeSeries(SMO_Handle p_handle, int nodeIndex, SMO_nodeAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length);

/*!
* \brief Retrieves link attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] linkIndex The index of the link
* \param[in] attr The link attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the link attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getLinkSeries(SMO_Handle p_handle, int linkIndex, SMO_linkAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length);

/*!
* \brief Retrieves system attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] attr The system attribute type to retrieve
* \param[in] startPeriod The starting time period to retrieve data from 
* \param[in] endPeriod The ending time period to retrieve data from
* \param[out] outValueArray Pointer to the system attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSystemSeries(SMO_Handle p_handle, SMO_systemAttribute attr, int startPeriod, int endPeriod, float **outValueArray, int *length);

/*!
* \brief Retrieves subcatchment attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The subcatchment attribute type to retrieve
* \param[out] outValueArray Pointer to the subcatchment attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSubcatchAttribute(SMO_Handle p_handle, int timeIndex, SMO_subcatchAttribute attr, float **outValueArray, int *length);

/*!
* \brief Retrieves node attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The node attribute type to retrieve
* \param[out] outValueArray Pointer to the node attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getNodeAttribute(SMO_Handle p_handle, int timeIndex, SMO_nodeAttribute attr, float **outValueArray, int *length);

/*!
* \brief Retrieves link attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The link attribute type to retrieve
* \param[out] outValueArray Pointer to the link attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getLinkAttribute(SMO_Handle p_handle, int timeIndex, SMO_linkAttribute attr, float **outValueArray, int *length);

/*!
* \brief Retrieves system attribute values for a given time period and attribute type
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] attr The system attribute type to retrieve
* \param[out] outValueArray Pointer to the system attribute values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSystemAttribute(SMO_Handle p_handle, int timeIndex, SMO_systemAttribute attr, float **outValueArray, int *length);

/*!
* \brief Retrieves subcatchment result values for a given time period
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] subcatchIndex The index of the subcatchment
* \param[out] outValueArray Pointer to the subcatchment result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSubcatchResult(SMO_Handle p_handle, int timeIndex, int subcatchIndex, float **outValueArray, int *length);

/*!
* \brief Retrieves node result values for a given time period
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] nodeIndex The index of the node
* \param[out] outValueArray Pointer to the node result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getNodeResult(SMO_Handle p_handle, int timeIndex, int nodeIndex, float **outValueArray, int *length);

/*!
* \brief Retrieves link result values for a given time period
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] linkIndex The index of the link
* \param[out] outValueArray Pointer to the link result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getLinkResult(SMO_Handle p_handle, int timeIndex, int linkIndex, float **outValueArray, int *length);

/*!
* \brief Retrieves system result values for a given time period
* \param[in] p_handle Pointer to a SMO_Handle
* \param[in] timeIndex The index of the time period
* \param[in] dummyIndex The index of the system
* \param[out] outValueArray Pointer to the system result values
* \param[out] length Pointer to the length of the outValueArray array
* \return Error code
*/
int EXPORT_OUT_API SMO_getSystemResult(SMO_Handle p_handle, int timeIndex, int dummyIndex, float **outValueArray, int *length);

/*!
* \brief Frees memory allocated by the API for the outValueArray
* \param[in] array Pointer to the outValueArray
*/
void EXPORT_OUT_API SMO_free(void **array);

/*!
* \brief Clears the error status of the SMO_Handle
* \param[in] p_handle Pointer to a SMO_Handle
*/
void EXPORT_OUT_API SMO_clearError(SMO_Handle p_handle_in);

/*!
* \brief Retrieves the error message from the SMO_Handle
* \param[in] p_handle Pointer to a SMO_Handle
* \param[out] msg_buffer Pointer to the error message
* \return Error code
*/
int EXPORT_OUT_API SMO_checkError(SMO_Handle p_handle_in, char **msg_buffer);

#ifdef __cplusplus
}
#endif

#endif /* SWMM_OUTPUT_H_ */
