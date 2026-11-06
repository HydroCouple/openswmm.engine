/**
 * @file      openswmm_output.h
 * @brief     Header file for SWMM output API.
 * @details Handles are owned from SMO_init through SMO_close. Unless a function
 * explicitly says otherwise, all output pointers must be non-null and handles must
 * be live with an opened file; null-handle branches are not consistently safe.
 * Result arrays and copied messages belong to the caller and use SMO_free.
 * Readers seek a shared stream, so serialize calls on each handle. File result
 * indices are zero-based and units come from the binary file, without conversion.
 * The error status is sticky until SMO_clearError; zero-return calls leave it intact.
 *
 * @date      Created: 2017-08-25
 * @date      Last edited: 2024-10-17
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef OPENSWMM_LEGACY_OUTPUT_H_
#define OPENSWMM_LEGACY_OUTPUT_H_

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
typedef void* SMO_Handle;

#include "openswmm_output_export.h"
#include "openswmm_output_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate an independent binary-output reader handle.
 *
 * Does not open a file. Initialize an empty slot; overwriting a live handle leaks its previous
 * allocation.
 *
 * @param[out] p_handle Non-null writable handle slot. Receives owned storage on success; unchanged
 *     if the primary allocation fails. Release with SMO_close.
 * @return Zero on primary allocation success, -1 otherwise. Error-manager allocation failure is not
 *     safely handled.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_init(SMO_Handle* p_handle);

/**
 * @brief Release reader metadata, close its file and destroy its handle.
 *
 * All handle-owned metadata becomes invalid. Arrays previously returned as caller-owned allocations
 * remain the caller responsibility; close does not free those arrays.
 *
 * @param[in,out] p_handle Non-null writable handle slot containing a live handle or null;
 *     successful destruction sets it to null.
 * @return Zero when a live handle is destroyed, -1 when the slot already contains null. File-close
 *     errors are ignored.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_close(SMO_Handle* p_handle);

/**
 * @brief Open and index a completed SWMM binary output file for reading.
 *
 * Fatal errors above 400 close the file but leave the handle owned by the caller for SMO_close.
 * Reads/index allocations are not uniformly checked, so a successful return does not certify every
 * byte of a corrupt file. Opening an already-open handle does not close its previous stream.
 *
 * @param[in,out] p_handle Non-null SMO_init handle not already owning an open file; metadata and
 *     stream are initialized in place.
 * @param[in] path Non-null null-terminated readable file path; use a length below MAXFILENAME for
 *     safe stored-name termination.
 * @return Zero on success, WARN10 when the file records a run error, -1 for a null handle, or a
 *     positive file/validation error (e.g. 434/435/436). WARN10 still allows metadata/results
 *     access.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_open(SMO_Handle p_handle, const char* path);

/**
 * @brief Read the model version integer from the file header.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] version Non-null writable integer slot.
 * @return Zero on success, -1 for null handle, or ERR436 on a short version read; nonzero status is
 *     stored in the handle.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVersion(SMO_Handle p_handle, int* version);

/**
 * @brief Allocate the five object counts for the opened output file.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] elementCount Non-null writable int-pointer slot, first set null; success returns five
 *     owned integers [subcatchments,nodes,links,systems=1,pollutants]. Free with SMO_free.
 * @param[out] length Non-null writable count slot, set to five even before allocation.
 * @return Zero on success or 414 on allocation failure; non-null handle is a strict precondition
 *     despite an internal null branch.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getProjectSize(SMO_Handle p_handle, int** elementCount,
                                                      int* length);

/**
 * @brief Allocate the file unit-system, flow-unit and concentration-unit flags.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] unitFlag Non-null writable pointer slot, first set null. Success returns owned
 *     [system,flow,pollutant0,...] flags; without pollutants the third flag is SMO_NONE. Free with
 *     SMO_free.
 * @param[out] length Non-null writable count: 2 + pollutant count, or three when no pollutants
 *     exist.
 * @return Zero on success or 414 on allocation failure. File-read counts are not checked; a live
 *     handle is required before even computing the result size.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getUnits(SMO_Handle p_handle, int** unitFlag, int* length);

/**
 * @brief Read the flow-unit flag from the output header.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] unitFlag Non-null writable integer, initialized to -1; receives SMO_CFS..SMO_MLD
 *     (0..5).
 * @return Zero after the read, or -1 for null handle; the read count is not checked, so zero does
 *     not guarantee a complete read.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getFlowUnits(SMO_Handle p_handle, int* unitFlag);

/**
 * @brief Allocate one concentration-unit flag per pollutant.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] unitFlag Non-null writable pointer slot receiving owned integer flags on success:
 *     SMO_MG, SMO_UG or SMO_COUNT. Free with SMO_free.
 * @param[out] length Non-null writable slot receiving the pollutant count on success.
 * @return Zero on success or 414 on allocation failure; zero-count malloc behavior is
 *     platform-dependent. File-read counts are not checked; outputs may remain unchanged on
 *     failure.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPollutantUnits(SMO_Handle p_handle, int** unitFlag,
                                                         int* length);

/**
 * @brief Return the output header start date.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[out] date Non-null writable slot, first set -1, then populated in absolute legacy SWMM
 *     decimal days.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getStartDate(SMO_Handle p_handle, double* date);

/**
 * @brief Return report-step seconds or the number of saved periods.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] code SMO_reportStep or SMO_numPeriods; other values yield ERR421.
 * @param[out] time Non-null writable integer slot, initialized to -1 before validation.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getTimes(SMO_Handle p_handle, SMO_time code, int* time);

/**
 * @brief Allocate the null-terminated ID of an output element.
 *
 * Names are loaded lazily into handle-owned metadata, then copied for the caller. Output slots are
 * not initialized on rejected arguments; allocation/read failures while loading/copying names are
 * not robustly reported.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type SMO_subcatch, SMO_node, SMO_link or SMO_pollut; system names are unsupported
 *     (ERR421).
 * @param[in] index Zero-based index strictly below the selected element count; invalid
 *     values yield ERR423.
 * @param[out] name Non-null writable pointer slot; receives an owned null-terminated copy on
 *     success. Free with SMO_free.
 * @param[out] length Non-null writable character count excluding the terminating null; allocated
 *     capacity is size + 1.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getElementName(SMO_Handle p_handle, SMO_elementType type,
                                                      int index, char** name, int* length);

/**
 * @brief Return the number of stored vars for an element type.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type SMO_subcatch, node, link or sys; unsupported types yield ERR421.
 * @param[out] count Non-null writable integer, initialized to -1 then assigned the selected count.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNumVars(SMO_Handle p_handle, SMO_elementType type,
                                                  int* count);

/**
 * @brief Translate a stored var position to its file code.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type Supported types: SMO_subcatch, node, link or sys.
 * @param[in] varIndex Zero-based position strictly below the selected stored count; invalid values
 *     yield ERR423.
 * @param[out] varCode Non-null writable integer initialized to -1; receives the file code on
 *     success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVarCode(SMO_Handle p_handle, SMO_elementType type,
                                                  int varIndex, int* varCode);

/**
 * @brief Allocate all stored var codes for an element type.
 *
 * Allocation success is not checked before copying; sufficient memory is a precondition for safe
 * completion. Output slots may remain unchanged on rejected type.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type Supported types: SMO_subcatch, node, link or sys; unsupported values yield
 *     ERR421.
 * @param[out] varCodes Non-null writable pointer slot receiving a new owned integer array; release
 *     with SMO_free.
 * @param[out] size Non-null writable number of returned entries, set after type selection.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVarCodes(SMO_Handle p_handle, SMO_elementType type,
                                                   int** varCodes, int* size);

/**
 * @brief Return the number of stored properties for an element type.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type SMO_subcatch, node or link; unsupported types yield ERR421.
 * @param[out] count Non-null writable integer, initialized to -1 then assigned the selected count.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNumProperties(SMO_Handle p_handle, SMO_elementType type,
                                                        int* count);

/**
 * @brief Translate a stored property position to its file code.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type Supported types: SMO_subcatch, node or link.
 * @param[in] propertyIndex Zero-based position strictly below the selected stored count; invalid
 *     values yield ERR423.
 * @param[out] propertyCode Non-null writable integer initialized to -1; receives the file code on
 *     success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyCode(SMO_Handle p_handle, SMO_elementType type,
                                                       int propertyIndex, int* propertyCode);

/**
 * @brief Allocate all stored property codes for an element type.
 *
 * Allocation success is not checked before copying; sufficient memory is a precondition for safe
 * completion. Output slots may remain unchanged on rejected type.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type Supported types: SMO_subcatch, node or link; unsupported values yield ERR421.
 * @param[out] propertyCodes Non-null writable pointer slot receiving a new owned integer array;
 *     release with SMO_free.
 * @param[out] size Non-null writable number of returned entries, set after type selection.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyCodes(SMO_Handle p_handle, SMO_elementType type,
                                                        int** propertyCodes, int* size);

/**
 * @brief Read one static model property from the output file.
 *
 * Seek/read results are not checked, so zero does not establish a complete read.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type SMO_subcatch, SMO_node or SMO_link; unsupported types yield ERR421.
 * @param[in] propertyIndex Zero-based stored property position, not its property code; strictly
 *     below that type property count.
 * @param[in] elementIndex Zero-based element index strictly below the type count; invalid positions
 *     yield ERR423.
 * @param[out] value Non-null writable float receiving the file-stored value in its property units;
 *     unchanged on rejected arguments.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyValue(SMO_Handle p_handle, SMO_elementType type,
                                                        int propertyIndex, int elementIndex,
                                                        float* value);

/**
 * @brief Allocate all static properties of one element in file order.
 *
 * Allocation and seek/read results are not checked. Property positions correspond to
 * SMO_getPropertyCodes; output slots may remain unchanged on rejection.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] type SMO_subcatch, SMO_node or SMO_link.
 * @param[in] elementIndex Zero-based element index strictly below its type count; invalid values
 *     yield ERR423.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable element-count slot; only rely on its value after success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyValues(SMO_Handle p_handle, SMO_elementType type,
                                                         int elementIndex, float** outValueArray,
                                                         int* length);

/**
 * @brief Allocate one subcatchment attribute over a half-open period interval.
 *
 * Invalid start/order yields ERR422; allocation failure yields ERR411. Reads are unchecked and
 * outputs are only assigned on success. All rates/depths/concentrations retain the file units.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] subcatchIndex Zero-based subcatchment index strictly below its count. The
 *     implementation check also admits index == count; callers must not use that invalid position.
 * @param[in] attr Zero-based stored subcatchment attribute position, including pollutant slots
 *     where present; must be below the stored variable count because it is not validated or
 *     code-mapped.
 * @param[in] startPeriod Inclusive zero-based saved-period index in [0,Nperiods).
 * @param[in] endPeriod Exclusive end index with startPeriod < endPeriod <= Nperiods; the upper
 *     bound is a caller precondition and is not checked.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable count, set to endPeriod - startPeriod on success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchSeries(SMO_Handle p_handle, int subcatchIndex,
                                                         SMO_subcatchAttribute attr,
                                                         int startPeriod, int endPeriod,
                                                         float** outValueArray, int* length);

/**
 * @brief Allocate one node attribute over a half-open period interval.
 *
 * Invalid start/order yields ERR422; allocation failure yields ERR411. Reads are unchecked and
 * outputs are only assigned on success. All rates/depths/concentrations retain the file units.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] nodeIndex Zero-based node index strictly below its count. The implementation check
 *     also admits index == count; callers must not use that invalid position.
 * @param[in] attr Zero-based stored node attribute position, including pollutant slots where
 *     present; must be below the stored variable count because it is not validated or code-mapped.
 * @param[in] startPeriod Inclusive zero-based saved-period index in [0,Nperiods).
 * @param[in] endPeriod Exclusive end index with startPeriod < endPeriod <= Nperiods; the upper
 *     bound is a caller precondition and is not checked.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable count, set to endPeriod - startPeriod on success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeSeries(SMO_Handle p_handle, int nodeIndex,
                                                     SMO_nodeAttribute attr, int startPeriod,
                                                     int endPeriod, float** outValueArray,
                                                     int* length);

/**
 * @brief Allocate one link attribute over a half-open period interval.
 *
 * Invalid start/order yields ERR422; allocation failure yields ERR411. Reads are unchecked and
 * outputs are only assigned on success. All rates/depths/concentrations retain the file units.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] linkIndex Zero-based link index strictly below its count. The implementation check
 *     also admits index == count; callers must not use that invalid position.
 * @param[in] attr Zero-based stored link attribute position, including pollutant slots where
 *     present; must be below the stored variable count because it is not validated or code-mapped.
 * @param[in] startPeriod Inclusive zero-based saved-period index in [0,Nperiods).
 * @param[in] endPeriod Exclusive end index with startPeriod < endPeriod <= Nperiods; the upper
 *     bound is a caller precondition and is not checked.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable count, set to endPeriod - startPeriod on success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkSeries(SMO_Handle p_handle, int linkIndex,
                                                     SMO_linkAttribute attr, int startPeriod,
                                                     int endPeriod, float** outValueArray,
                                                     int* length);

/**
 * @brief Allocate one system attribute over a half-open period interval.
 *
 * Invalid start/order yields ERR423; allocation failure yields ERR411. Reads are unchecked and
 * outputs are only assigned on success. All rates/depths/concentrations retain the file units.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] attr Zero-based stored system attribute position, including pollutant slots where
 *     present; must be below the stored variable count because it is not validated or code-mapped.
 * @param[in] startPeriod Inclusive zero-based saved-period index in [0,Nperiods).
 * @param[in] endPeriod Exclusive end index with startPeriod < endPeriod <= Nperiods; the upper
 *     bound is a caller precondition and is not checked.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable count, set to endPeriod - startPeriod on success.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemSeries(SMO_Handle p_handle,
                                                       SMO_systemAttribute attr, int startPeriod,
                                                       int endPeriod, float** outValueArray,
                                                       int* length);

/**
 * @brief Allocate one attribute for all subcatchment entries at a saved period.
 *
 * Allocation failure yields ERR411; individual seek/read success is not checked. Output
 * pointer/count are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); invalid values yield ERR422.
 * @param[in] attr Unchecked zero-based stored subcatchment variable position; must be below its
 *     variable count.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable number of returned subcatchment entries in file index order.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchAttribute(SMO_Handle p_handle, int periodIndex,
                                                            SMO_subcatchAttribute attr,
                                                            float** outValueArray, int* length);

/**
 * @brief Allocate one attribute for all node entries at a saved period.
 *
 * Allocation failure yields ERR411; individual seek/read success is not checked. Output
 * pointer/count are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); invalid values yield ERR422.
 * @param[in] attr Unchecked zero-based stored node variable position; must be below its variable
 *     count.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable number of returned node entries in file index order.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeAttribute(SMO_Handle p_handle, int periodIndex,
                                                        SMO_nodeAttribute attr,
                                                        float** outValueArray, int* length);

/**
 * @brief Allocate one attribute for all link entries at a saved period.
 *
 * Allocation failure yields ERR411; individual seek/read success is not checked. Output
 * pointer/count are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); invalid values yield ERR422.
 * @param[in] attr Unchecked zero-based stored link variable position; must be below its variable
 *     count.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable number of returned link entries in file index order.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkAttribute(SMO_Handle p_handle, int periodIndex,
                                                        SMO_linkAttribute attr,
                                                        float** outValueArray, int* length);

/**
 * @brief Allocate one attribute for all system entries at a saved period.
 *
 * Allocation failure yields ERR411; individual seek/read success is not checked. Output
 * pointer/count are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); invalid values yield ERR422.
 * @param[in] attr Unchecked zero-based stored system variable position; must be below its variable
 *     count.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] length Non-null writable number of returned entries (one for the system).
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemAttribute(SMO_Handle p_handle, int periodIndex,
                                                          SMO_systemAttribute attr,
                                                          float** outValueArray, int* length);

/**
 * @brief Allocate all stored variables for one subcatchment at a period.
 *
 * Invalid period yields ERR422, invalid element yields ERR423, allocation failure ERR411. Seek/read
 * counts are not checked; result slots are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); valid bounds are required.
 * @param[in] subcatchIndex Zero-based subcatchment index strictly below its count. The internal
 *     test wrongly admits equality, so the caller must enforce the strict bound.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] arrayLength Non-null writable stored variable count for this type; values are in file
 *     position order, including pollutant entries.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchResult(SMO_Handle p_handle, int periodIndex,
                                                         int subcatchIndex, float** outValueArray,
                                                         int* arrayLength);

/**
 * @brief Allocate all stored variables for one node at a period.
 *
 * Invalid period yields ERR422, invalid element yields ERR423, allocation failure ERR411. Seek/read
 * counts are not checked; result slots are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); valid bounds are required.
 * @param[in] nodeIndex Zero-based node index strictly below its count. The internal test wrongly
 *     admits equality, so the caller must enforce the strict bound.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] arrayLength Non-null writable stored variable count for this type; values are in file
 *     position order, including pollutant entries.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeResult(SMO_Handle p_handle, int periodIndex,
                                                     int nodeIndex, float** outValueArray,
                                                     int* arrayLength);

/**
 * @brief Allocate all stored variables for one link at a period.
 *
 * Invalid period yields ERR422, invalid element yields ERR423, allocation failure ERR411. Seek/read
 * counts are not checked; result slots are only assigned on success.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); valid bounds are required.
 * @param[in] linkIndex Zero-based link index strictly below its count. The internal test wrongly
 *     admits equality, so the caller must enforce the strict bound.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] arrayLength Non-null writable stored variable count for this type; values are in file
 *     position order, including pollutant entries.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkResult(SMO_Handle p_handle, int periodIndex,
                                                     int linkIndex, float** outValueArray,
                                                     int* arrayLength);

/**
 * @brief Allocate all stored variables for one system at a period.
 *
 * The implementation executes its read block even after validation/allocation errors. A valid live
 * handle, valid timeIndex and successful allocation are strict safety preconditions; errors do not
 * provide a safe recovery path.
 *
 * @param[in] p_handle Live non-null SMO_init handle with a successfully opened output file;
 *     borrowed, not retained anew. Calls sharing a handle must be serialized because they seek its
 *     FILE stream.
 * @param[in] periodIndex Zero-based saved period in [0,Nperiods); valid bounds are required.
 * @param[in] dummyIndex Ignored; there is one system record.
 * @param[out] outValueArray Non-null writable pointer slot; receives newly allocated float storage
 *     on success. Caller releases it with SMO_free; values retain the output file attribute units.
 * @param[out] arrayLength Non-null writable stored variable count for this type; values are in file
 *     position order, including pollutant entries.
 * @return Zero on success, a positive output-API error code on rejected arguments/allocation, with
 *     status stored in the handle. Successful calls do not clear a previous stored error.
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemResult(SMO_Handle p_handle, int periodIndex,
                                                       int dummyIndex, float** outValueArray,
                                                       int* arrayLength);

/**
 * @brief Release a caller-owned allocation returned by this API.
 *
 * Does not accept an arbitrary interior pointer or the SMO_Handle allocation; destroy handles with
 * SMO_close. Every caller-owned array/message is released separately.
 *
 * @param[in,out] array Optional pointer to an API-allocated array or message pointer. If non-null,
 *     frees *array (which may be null) and sets it to null.
 */
void EXPORT_OPENSWMMCORE_OUTPUT_API SMO_free(void** array);

/**
 * @brief Reset the handle sticky error status to zero.
 *
 * @param[in,out] p_handle Live non-null initialized handle; no open file is required. Null is
 *     unchecked.
 */
void EXPORT_OPENSWMMCORE_OUTPUT_API SMO_clearError(SMO_Handle p_handle);

/**
 * @brief Copy the sticky output-reader error message into a new allocation.
 *
 * Does not clear the status. Successful read operations do not clear earlier errors; call
 * SMO_clearError explicitly. The output string uses the reader lookup table and can be empty for an
 * unknown code.
 *
 * @param[in] p_handle Live initialized handle, or null for a -1 return; borrowed.
 * @param[out] msg_buffer Non-null writable char-pointer slot; success writes a new owned message
 *     when status is nonzero, otherwise null. Free returned storage with SMO_free; allocation
 *     failure can also produce null.
 * @return Current sticky status, zero for no stored error, or -1 for a null handle (message slot
 *     then unchanged).
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_checkError(SMO_Handle p_handle, char** msg_buffer);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_LEGACY_OUTPUT_H_ */
