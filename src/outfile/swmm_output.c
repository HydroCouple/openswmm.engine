/*!
* \file swmm_output.c
* \brief Source code providing an API for reading SWMM binary output files.
* \author Colleen Barr (US EPA - ORD/NHEERL)
* \author  Michael E. Tryby (US EPA - ORD/NHEERL) (Modified)
* \author  Bryant McDonnell (Modified)
* \date Created: 2017-08-25
* \date Last edited: 2024-10-17
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errormanager.h"

#include "messages.h"
#include "swmm_output.h"


// NOTE: These depend on machine data model and may change when porting
#ifdef _WIN32    // Windows (32-bit and 64-bit)
/*!
* \def F_OFF
* \brief 8 byte / 64 bit integer for large file support on Windows (32-bit and 64-bit)
* \note This is a Microsoft specific type. 
*/
#define F_OFF __int64
#else
/*!
* \def F_OFF
* \brief 8 byte / 64 bit integer for large file support on Unix-like systems
*/
#define F_OFF off_t
#endif

/*!
* \def INT4
* \brief Must be a 4 byte / 32 bit integer type
*/
#define INT4 int

/*!
* \def REAL4
* \brief Must be a 4 byte / 32 bit real type
*/
#define REAL4 float

/*!
* \def RECORDSIZE
* \brief Memory alignment 4 byte word size for both int and real types
*/
#define RECORDSIZE 4


/*!
* \def DATASIZE
* \brief Dates are stored as 8 byte word size
*/
#define DATESIZE 8

/*!
* \def NELEMENTTYPES
* \brief Number of element types
*/
#define NELEMENTTYPES 5

/*!
* \def MEMCHECK 
* \brief Check if memory allocation was successful
*/
#define MEMCHECK(x) (((x) == NULL) ? 414 : 0)

/*!
* \struct IDentry
* \brief Structure for element names
*/
struct IDentry {
	/*! \brief Pointer to element name */ 
    char* IDname;
	/*! \brief Length of element name */
    int   length;
};
typedef struct IDentry idEntry;


/*!
* \struct data
* \brief Structure for SWMM binary output file data
*/
typedef struct {
	/*! \brief File path/name */
    char  name[MAXFILENAME + 1];

	/*! \brief File pointer */
    FILE* file;

	/*! \brief Array of pointers to element names */
    struct IDentry* elementNames;

	/*! \brief Number of reporting periods */
    long Nperiods;

	/*! \brief Flow units code */
    int  FlowUnits;

	/*! \brief Number of subcatchments */
    int Nsubcatch;

	/*! \brief Number of nodes */
    int Nnodes;

	/*! \brief Number of links */
    int Nlinks;

	/*! \brief Number of pollutants */
    int Npolluts;

	/*! \brief Number of subcatch reporting variables */
    int SubcatchVars;

	/*! \brief Number of node reporting variables */
    int NodeVars;

	/*! \brief Number of link reporting variables */
    int LinkVars;

	/*! \brief Number of system reporting variables */
    int SysVars;

	/*! \brief Start date of simulation */
    double StartDate;

	/*! \brief Reporting time step (seconds) */
    int    ReportStep;

	/*! \brief File position where object ID names start */
    F_OFF IDPos;

	/*! \brief File position where object properties start */
    F_OFF ObjPropPos;

	/*! \brief File position where results start */
    F_OFF ResultsPos;

	/*! \brief Number of bytes used for results in each period */
    F_OFF BytesPerPeriod;

	/*! \brief Pointer to error manager handle */
    error_handle_t* error_handle;
} data_t;


//-----------------------------------------------------------------------------
//   Local functions
//-----------------------------------------------------------------------------

/*!
* \brief Error lookup function
* \param[in] errcode Error code
* \param[out] errmsg Error message
* \param[in] length Length of error message
*/
void errorLookup(int errcode, char *errmsg, int length);

/*!
* \brief Validate the output file
* \param[in] p_data Pointer to data structure
* \return Error code
*/
int  validateFile(data_t *p_data);

/*!
* \brief Initialize element names
* \param[in] p_data Pointer to data structure
*/
void initElementNames(data_t *p_data);

/*!
* \brief Get time value
* \param[in] p_data Pointer to data structure
* \param[in] timeIndex Time index
* \return Time value
*/
double getTimeValue(data_t *p_data, int timeIndex);

/*!
* \brief Get subcatchment value
* \param[in] p_data Pointer to data structure
* \param[in] timeIndex Time index
* \param[in] subcatchIndex Subcatchment index
* \param[in] attr Subcatchment attribute
* \return Subcatchment value
*/
float  getSubcatchValue(data_t *p_data, int timeIndex, int subcatchIndex, SMO_subcatchAttribute attr);

/*!
* \brief Get node value 
* \param[in] p_data Pointer to data structure
* \param[in] timeIndex Time index
* \param[in] nodeIndex Node index
* \param[in] attr Node attribute
* \return Node value
*/
float  getNodeValue(data_t *p_data, int timeIndex, int nodeIndex, SMO_nodeAttribute attr);

/*!
* \brief Get link value
* \param[in] p_data Pointer to data structure
* \param[in] timeIndex Time index
* \param[in] linkIndex Link index
* \param[in] attr Link attribute
* \return Link value
*/
float  getLinkValue(data_t *p_data, int timeIndex, int linkIndex, SMO_linkAttribute attr);

/*!
* \brief Get system value
* \param[in] p_data Pointer to data structure
* \param[in] timeIndex Time index
* \param[in] attr System attribute
* \return System value
*/
float  getSystemValue(data_t *p_data, int timeIndex, SMO_systemAttribute attr);

/*!
* \brief Open file for reading with error handling 
* \param[in] f Pointer to file pointer
* \param[in] name File path/name
* \param[in] mode File mode
* \return Error
* \note This function is a wrapper for fopen
*/
int   _fopen(FILE **f, const char *name, const char *mode);

/*!
* \brief Seek to position in file with error handling
* \param[in] stream Pointer to file stream
* \param[in] offset Offset from whence
* \param[in] whence Position in file
* \return Error
* \note This function is a wrapper for fseek
*/
int   _fseek(FILE *stream, F_OFF offset, int whence);

/*!
* \brief Get current position in file with error handling
* \param[in] stream Pointer to file stream
* \return Current position in file
* \note This function is a wrapper for ftell
*/
F_OFF _ftell(FILE *stream);

/*!
* \brief Allocate memory for float array
* \param[in] n Number of elements
* \return Pointer to float array
*/
float *newFloatArray(int n);

/*!
* \brief Allocate memory for integer array
* \param[in] n Number of elements
* \return Pointer to integer array
*/
int   *newIntArray(int n);

/*!
* \brief Allocate memory for character array
* \param[in] n Number of elements
* \return Pointer to character array
*/
char  *newCharArray(int n);


/*!
* \brief Initialize pointer for the opaque SMO_Handle.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \return Error code 0 on success, -1 on failure
* \note The existence of this function has been carefully considered. Don't change it.
* \todo Need to handle errors during initialization better.
*/
int EXPORT_OUT_API SMO_init(SMO_Handle *p_handle)
{
    int     errorcode = 0;
    data_t *priv_data = NULL;

    // // Allocate memory for private data
    priv_data = (data_t *)calloc(1, sizeof(data_t));

    if (priv_data != NULL) {
        
        priv_data->elementNames = NULL;
        priv_data->file = NULL;

        priv_data->error_handle = new_error_manager(errorLookup);
        
        *p_handle = priv_data;
    } else
        errorcode = -1;

    // TODO: Need to handle errors during initialization better.
    return errorcode;
}


/*!
* \brief Clean up after and close Output API
* \param[in] p_handle Pointer to opaque SMO_Handle
* \return Error code 0 on success, -1 on failure
*/
int EXPORT_OUT_API SMO_close(SMO_Handle *p_handle)
{
    data_t *p_data;
    int i, n, errorcode = 0;

    p_data = (data_t *)*p_handle;

    if (p_data == NULL)
        errorcode = -1;

    else {
        if (p_data->elementNames != NULL) {
            n = p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks +
                p_data->Npolluts;

            for (i = 0; i < n; i++)
                free(p_data->elementNames[i].IDname);

            free(p_data->elementNames);
        }

        dst_error_manager(p_data->error_handle);

        if (p_data->file != NULL)
            fclose(p_data->file);

        free(p_data);

        *p_handle = NULL;
    }

    return errorcode;
}

/*!
* \brief Open the output binary file and read the header.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] path File path/name
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_open(SMO_Handle p_handle, const char *path)
{
    int   err, errorcode = 0;
    F_OFF offset;

    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        return -1;
    else {

#ifdef _MSC_VER
        strncpy_s(p_data->name, MAXFILENAME, path, MAXFILENAME);
#else
        strncpy(p_data->name, path, MAXFILENAME);
#endif

        // Attempt to open binary output file for reading only
        if ((_fopen(&(p_data->file), path, "rb")) != 0)
            errorcode = 434;

        // --- validate the output file
        else if ((err = validateFile(p_data)) != 0)
            errorcode = err;

        // If a warning is encountered read file header
        if (errorcode < 400) {
            // --- otherwise read additional parameters from start of file
            fseek(p_data->file, 3 * RECORDSIZE, SEEK_SET);
            fread(&(p_data->Nsubcatch), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Nnodes), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Nlinks), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Npolluts), RECORDSIZE, 1, p_data->file);

            // Compute offset for saved subcatch/node/link input values
            offset =
                (p_data->Nsubcatch + 2) * RECORDSIZE    // Subcatchment area
                + (3 * p_data->Nnodes + 4) *
                      RECORDSIZE    // Node type, invert & max depth
                + (5 * p_data->Nlinks + 6) *
                      RECORDSIZE;    // Link type, z1, z2, max depth & length
            offset += p_data->ObjPropPos;

            // Read number & codes of computed variables
            _fseek(p_data->file, offset, SEEK_SET);
            fread(&(p_data->SubcatchVars), RECORDSIZE, 1,
                  p_data->file);    // # Subcatch variables

            _fseek(p_data->file, p_data->SubcatchVars * RECORDSIZE, SEEK_CUR);
            fread(&(p_data->NodeVars), RECORDSIZE, 1,
                  p_data->file);    // # Node variables

            _fseek(p_data->file, p_data->NodeVars * RECORDSIZE, SEEK_CUR);
            fread(&(p_data->LinkVars), RECORDSIZE, 1,
                  p_data->file);    // # Link variables

            _fseek(p_data->file, p_data->LinkVars * RECORDSIZE, SEEK_CUR);
            fread(&(p_data->SysVars), RECORDSIZE, 1,
                  p_data->file);    // # System variables

            // --- read data just before start of output results
            offset = p_data->ResultsPos - 3 * RECORDSIZE;
            _fseek(p_data->file, offset, SEEK_SET);
            fread(&(p_data->StartDate), DATESIZE, 1, p_data->file);
            fread(&(p_data->ReportStep), RECORDSIZE, 1, p_data->file);

            // --- compute number of bytes of results values used per time
            // period
            p_data->BytesPerPeriod =
                DATESIZE +
                (p_data->Nsubcatch * p_data->SubcatchVars +
                 p_data->Nnodes * p_data->NodeVars +
                 p_data->Nlinks * p_data->LinkVars + p_data->SysVars) *
                    RECORDSIZE;
        }
    }
    // If error close the binary file
    if (errorcode > 400) {
        set_error(p_data->error_handle, errorcode);
        SMO_close(&p_handle);
    }

    return errorcode;
}

/*!
* \brief Get the SWMM version that wrote the binary file.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] version SWMM version
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getVersion(SMO_Handle p_handle, int *version)
{
    int     errorcode = 0;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        return -1;
    else {
        fseek(p_data->file, 1 * RECORDSIZE, SEEK_SET);
        if (fread(version, RECORDSIZE, 1, p_data->file) != 1)
            errorcode = 436;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get project size.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] elementCount Array of element counts
* \param[out] length Length of elementCount array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getProjectSize(SMO_Handle p_handle, int **elementCount, int *length)
{
    int     errorcode = 0;
    int     *temp;
    data_t  *p_data;

    p_data = (data_t*)p_handle;

    *elementCount = NULL;
    *length = NELEMENTTYPES;

    if (p_data == NULL)
        errorcode = -1;
    else if (MEMCHECK(temp = newIntArray(*length)))
        errorcode = 414;
    else {
        temp[0] = p_data->Nsubcatch;
        temp[1] = p_data->Nnodes;
        temp[2] = p_data->Nlinks;
        temp[3] = 1;   // NSystems
        temp[4] = p_data->Npolluts;

        *elementCount = temp;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get unit system.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] unitFlag Unit system flag
* \param[out] length Length of unitFlag array
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getUnits(SMO_Handle p_handle, int **unitFlag, int *length)
{
    int     errorcode = 0;
    int*    temp;
    F_OFF   offset;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    *unitFlag = NULL;
    if (p_data->Npolluts > 0)
        *length = 2 + p_data->Npolluts;
    else
        *length = 3;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (MEMCHECK(temp = newIntArray(*length)))
        errorcode = 414;
    else {
        // Set flow units flag
        fseek(p_data->file, 2*RECORDSIZE, SEEK_SET);
        fread(&temp[1], RECORDSIZE, 1, p_data->file);

        // Set unit system based on flow flag
        if (temp[1] < SMO_CMS)
            temp[0] = SMO_US;
        else
            temp[0] = SMO_SI;

        // Set conc units flag
        if (p_data->Npolluts == 0)
            temp[2] = SMO_NONE;
        else {
            offset = p_data->ObjPropPos - (p_data->Npolluts * RECORDSIZE);
            _fseek(p_data->file, offset, SEEK_SET);
            fread(&temp[2], RECORDSIZE, p_data->Npolluts, p_data->file);
        }
        *unitFlag = temp;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Returns unit flag for flow
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] unitFlag Flow unit flag
* \returns 0: CFS (cubic feet per second), 
*          1: GPM (gallons per minute), 
*          2: MGD (million gallons per day), 
*          3: CMS (cubic meters per second), 
*          4: LPS (liters per second), 
*          5: MLD (million liters per day)
*/
int EXPORT_OUT_API SMO_getFlowUnits(SMO_Handle p_handle, int *unitFlag)
{
    int     errorcode = 0;
    data_t* p_data;

    *unitFlag = -1;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        return -1;
    else {
        fseek(p_data->file, 2 * RECORDSIZE, SEEK_SET);
        fread(unitFlag, RECORDSIZE, 1, p_data->file);
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Returns unit flag for pollutant. Concentration units are located after the pollutant ID
* names and before the object properties start, and are stored for each pollutant. They're stored
* as 4-byte integers with the following codes:
* 0: mg/L
* 1: ug/L
* 2: count/L
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] unitFlag Array of unit flags
* \param[out] length Length of unitFlag array
* \returns Error code
* \note Valid values are 0 to Npolluts-1
*/
int EXPORT_OUT_API SMO_getPollutantUnits(SMO_Handle p_handle, int **unitFlag, int *length)
{
    int    errorcode = 0;
    int    *temp;
    F_OFF  offset;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (MEMCHECK(temp = newIntArray(p_data->Npolluts)))
        errorcode = 414;
    else {
        offset = p_data->ObjPropPos - (p_data->Npolluts * RECORDSIZE);
        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->Npolluts, p_data->file);

        *unitFlag = temp;
        *length   = p_data->Npolluts;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get start date.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] date Start date
* \return Error code 0 on success, -1 on failure
*/
int EXPORT_OUT_API SMO_getStartDate(SMO_Handle p_handle, double *date)
{
    int     errorcode = 0;
    data_t *p_data;

    *date = -1.0;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        *date = p_data->StartDate;

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get report step and number of periods.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] code Time code (SMO_reportStep or SMO_numPeriods)
* \param[out] time Time value (report step or number of periods)
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getTimes(SMO_Handle p_handle, SMO_time code, int *time)
{
    int    errorcode = 0;
    data_t *p_data;

    *time = -1;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else {
        switch (code) {
            case SMO_reportStep:
                *time = p_data->ReportStep;
                break;
            case SMO_numPeriods:
                *time = p_data->Nperiods;
                break;
            default:
                errorcode = 421;
        }
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get element name by index.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] type Element type
* \param[in] index Element index
* \param[out] name Element name
* \param[out] length Length of element name
*/
int EXPORT_OUT_API SMO_getElementName(SMO_Handle p_handle, SMO_elementType type,
    int index, char **name, int *length)
{
    int    idx = -1, errorcode = 0;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = 410;
    else if (p_data->file == NULL)
        errorcode = 411;
    else {
        // Initialize the name array if necessary
        if (p_data->elementNames == NULL)
            initElementNames(p_data);

        switch (type) {
            case SMO_subcatch:
                if (index < 0 || index >= p_data->Nsubcatch)
                    errorcode = 423;
                else
                    idx = index;
                break;

            case SMO_node:
                if (index < 0 || index >= p_data->Nnodes)
                    errorcode = 423;
                else
                    idx = p_data->Nsubcatch + index;
                break;

            case SMO_link:
                if (index < 0 || index >= p_data->Nlinks)
                    errorcode = 423;
                else
                    idx = p_data->Nsubcatch + p_data->Nnodes + index;
                break;

            case SMO_pollut:
                if (index < 0 || index >= p_data->Npolluts)
                    errorcode = 423;
                else
                    idx = p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks +
                          index;
                break;

            default:
                errorcode = 421;
        }

        if (!errorcode) {
            *length = p_data->elementNames[idx].length;
            *name   = newCharArray(*length + 1);
            // Writes IDname and an additional null character to name


#ifdef _MSC_VER
            strncpy_s(
                *name, 
                (*length + 1) * sizeof(char),
                p_data->elementNames[idx].IDname,
                (*length + 1) * sizeof(char));
#else
            strncpy(*name, p_data->elementNames[idx].IDname,
                (*length + 1) * sizeof(char));
#endif

        }
    }

    return set_error(p_data->error_handle, errorcode);
}


/*!
* \brief Get subcatchment time series results for particular attribute. Specify series
* start and length using timeIndex and length respectively.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] subcatchIndex Subcatchment index
* \param[in] attr Subcatchment attribute
* \param[in] startPeriod Start period
* \param[in] endPeriod End period
* \param[out] outValueArray Array of values
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSubcatchSeries(SMO_Handle p_handle, int subcatchIndex,
    SMO_subcatchAttribute attr, int startPeriod, int endPeriod,
    float **outValueArray, int *length)
{
    int    k, len, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (subcatchIndex < 0 || subcatchIndex > p_data->Nsubcatch)
        errorcode = 420;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods ||
             endPeriod <= startPeriod)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(len = endPeriod - startPeriod))
    errorcode = 411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] =
                getSubcatchValue(p_data, startPeriod + k, subcatchIndex, attr);

        *outValueArray = temp;
        *length         = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get node time series results for particular attribute. Specify series
* start and length using timeIndex and length respectively.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] nodeIndex Node index
* \param[in] attr Node attribute
* \param[in] startPeriod Start period
* \param[in] endPeriod End period
* \param[out] outValueArray Array of values
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getNodeSeries(SMO_Handle p_handle, int nodeIndex,
    SMO_nodeAttribute attr, int startPeriod, int endPeriod,
    float **outValueArray, int *length)
{
    int    k, len, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (nodeIndex < 0 || nodeIndex > p_data->Nnodes)
        errorcode = 420;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods ||
             endPeriod <= startPeriod)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(len = endPeriod - startPeriod))
    errorcode = 411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getNodeValue(p_data, startPeriod + k, nodeIndex, attr);

        *outValueArray = temp;
        *length         = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Gets the time series results for a particular attribute for a link. Specify series
* start and length using timeIndex and length respectively.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] linkIndex Link index
* \param[in] attr Link attribute
* \param[in] startPeriod Start period index 
* \param[in] endPeriod End period index 
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getLinkSeries(SMO_Handle p_handle, int linkIndex,
    SMO_linkAttribute attr, int startPeriod, int endPeriod,
    float **outValueArray, int *length)
{
    int    k, len, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (linkIndex < 0 || linkIndex > p_data->Nlinks)
        errorcode = 420;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods ||
             endPeriod <= startPeriod)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(len = endPeriod - startPeriod))
    errorcode = 411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getLinkValue(p_data, startPeriod + k, linkIndex, attr);

        *outValueArray = temp;
        *length         = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get system time series results for particular attribute. Specify series
* start and length using timeIndex and length respectively.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] attr System attribute 
* \param[in] startPeriod Start period index
* \param[in] endPeriod End period index
* \param[out] outValueArray Array of values to use to store the results 
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSystemSeries(SMO_Handle p_handle, SMO_systemAttribute attr,
    int startPeriod, int endPeriod, float **outValueArray, int *length)
{
    int    k, len, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods ||
             endPeriod <= startPeriod)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(len = endPeriod - startPeriod))
    errorcode = 411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getSystemValue(p_data, startPeriod + k, attr);

        *outValueArray = temp;
        *length         = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get attribute for all subcatchments at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index 
* \param[in] attr Subcatchment attribute
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSubcatchAttribute(SMO_Handle p_handle, int periodIndex,
    SMO_subcatchAttribute attr, float **outValueArray, int *length)
{
    int    k, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(p_data->Nsubcatch)) errorcode = 411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nsubcatch; k++)
            temp[k] = getSubcatchValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length        = p_data->Nsubcatch;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get attribute for all nodes at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] attr Node attribute
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getNodeAttribute(SMO_Handle p_handle, int periodIndex,
    SMO_nodeAttribute attr, float **outValueArray, int *length)
{
    int    k, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(p_data->Nnodes)) errorcode = 411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nnodes; k++)
            temp[k] = getNodeValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length        = p_data->Nnodes;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get attribute for all links at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] attr Link attribute
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getLinkAttribute(SMO_Handle p_handle, int periodIndex,
    SMO_linkAttribute attr, float **outValueArray, int *length)
{
    int    k, errorcode = 0;
    float  *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    // Check memory for outValues
    else if
        MEMCHECK(temp = newFloatArray(p_data->Nlinks)) errorcode = 411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nlinks; k++)
            temp[k] = getLinkValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length        = p_data->Nlinks;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get attribute for the system at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index 
* \param[in] attr System attribute
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSystemAttribute(SMO_Handle p_handle, int periodIndex,
    SMO_systemAttribute attr, float **outValueArray, int *length)
{
    int     errorcode = 0;
    float   *temp;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if
        MEMCHECK(temp = newFloatArray(1)) errorcode = 411;
    else {
        // don't need to loop since there's only one system
        temp[0] = getSystemValue(p_data, periodIndex, attr);

        *outValueArray = temp;
        *length        = 1;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get subcatchment result for all attributes at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] subcatchIndex Subcatchment index
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSubcatchResult(SMO_Handle p_handle, int periodIndex,
    int subcatchIndex, float **outValueArray, int *arrayLength)
{
    int    errorcode = 0;
    float  *temp;
    F_OFF  offset;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if (subcatchIndex < 0 || subcatchIndex > p_data->Nsubcatch)
        errorcode = 423;
    else if
        MEMCHECK(temp = newFloatArray(p_data->SubcatchVars)) errorcode = 411;
    else {
        // --- compute offset into output file
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod +
                 2 * RECORDSIZE;
        // add offset for subcatchment
        offset += (subcatchIndex * p_data->SubcatchVars) * RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->SubcatchVars, p_data->file);

        *outValueArray = temp;
        *arrayLength   = p_data->SubcatchVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get node result for all attributes at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] nodeIndex Node index
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getNodeResult(SMO_Handle p_handle, int periodIndex,
    int nodeIndex, float **outValueArray, int *arrayLength)
{
    int    errorcode = 0;
    float  *temp;
    F_OFF  offset;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if (nodeIndex < 0 || nodeIndex > p_data->Nnodes)
        errorcode = 423;
    else if
        MEMCHECK(temp = newFloatArray(p_data->NodeVars)) errorcode = 411;
    else {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod +
                 2 * RECORDSIZE;
        // add offset for subcatchment and node
        offset += (p_data->Nsubcatch * p_data->SubcatchVars +
                   nodeIndex * p_data->NodeVars) *
                  RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->NodeVars, p_data->file);

        *outValueArray = temp;
        *arrayLength   = p_data->NodeVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get link result for all attributes at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] linkIndex Link index
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getLinkResult(SMO_Handle p_handle, int periodIndex,
    int linkIndex, float **outValueArray, int *arrayLength)
{
    int    errorcode = 0;
    float  *temp;
    F_OFF  offset;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if (linkIndex < 0 || linkIndex > p_data->Nlinks)
        errorcode = 423;
    else if
        MEMCHECK(temp = newFloatArray(p_data->LinkVars)) errorcode = 411;
    else {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod +
                 2 * RECORDSIZE;
        // add offset for subcatchment and node and link
        offset +=
            (p_data->Nsubcatch * p_data->SubcatchVars +
             p_data->Nnodes * p_data->NodeVars + linkIndex * p_data->LinkVars) *
            RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->LinkVars, p_data->file);

        *outValueArray = temp;
        *arrayLength   = p_data->LinkVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Get system result for all attributes at a given time.
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[in] periodIndex Time index
* \param[in] dummyIndex Dummy index
* \param[out] outValueArray Array of values to use to store the results
* \param[out] length Length of outValueArray
* \return Error code 0 on success, -1 on failure or error code
*/
int EXPORT_OUT_API SMO_getSystemResult(SMO_Handle p_handle, int periodIndex,
    int dummyIndex, float **outValueArray, int *arrayLength)
{
    int    errorcode = 0;
    float  *temp;
    F_OFF  offset;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if
        MEMCHECK(temp = newFloatArray(p_data->SysVars)) errorcode = 411;
    {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod +
                 2 * RECORDSIZE;
        // add offset for subcatchment and node and link (system starts after
        // the last link)
        offset += (p_data->Nsubcatch * p_data->SubcatchVars +
                   p_data->Nnodes * p_data->NodeVars +
                   p_data->Nlinks * p_data->LinkVars) *
                  RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->SysVars, p_data->file);

        *outValueArray = temp;
        *arrayLength   = p_data->SysVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/*!
* \brief Frees memory allocated by API calls
* \param[in] array Pointer to array
*/
void EXPORT_OUT_API SMO_free(void **array)
{
    if (array != NULL) {
        free(*array);
        *array = NULL;
    }
}

/*!
* \brief Clears error message stored in the error handle
* \param[in] p_handle Pointer to opaque SMO_Handle
*/
void EXPORT_OUT_API SMO_clearError(SMO_Handle p_handle) {
    data_t *p_data;

    p_data = (data_t *)p_handle;
    clear_error(p_data->error_handle);
}

/*!
* \brief Checks for error in the error handle and copies the error message to the
* message buffer 
* \param[in] p_handle Pointer to opaque SMO_Handle
* \param[out] msg_buffer Error message buffer 
* \return Error code
*/
int EXPORT_OUT_API SMO_checkError(SMO_Handle p_handle, char **msg_buffer) {
    int    errorcode = 0;
    char   *temp      = NULL;
    data_t *p_data;

    p_data = (data_t *)p_handle;

    if (p_data == NULL)
        return -1;
    else {
        errorcode = p_data->error_handle->error_status;
        if (errorcode)
            temp = check_error(p_data->error_handle);

        *msg_buffer = temp;
    }

    return errorcode;
}

/*!
* \brief Takes error code and returns error message
* \param[in] errcode Error code 
* \param[out] dest_msg Destination message buffer
* \param[in] dest_len Length of destination message buffer
*/
void errorLookup(int errcode, char *dest_msg, int dest_len)
{
    const char *msg;

    switch (errcode) {
        case 10:
            msg = WARN10;
            break;
        case 411:
            msg = ERR411;
            break;
        case 421:
            msg = ERR421;
            break;
        case 422:
            msg = ERR422;
            break;
        case 423:
            msg = ERR423;
            break;
        case 424:
            msg = ERR424;
            break;
        case 434:
            msg = ERR434;
            break;
        case 435:
            msg = ERR435;
            break;
        case 436:
            msg = ERR436;
            break;
        default:
            msg = ERR440;
    }

#ifdef _MSC_VER
    strncpy_s(dest_msg, MAXMSG, msg, MAXMSG);
#else
    strncpy(dest_msg, msg, MAXMSG);
#endif

}

/*!
* \addtogroup SWMM_Output_API_Local_Functions SWMM Output API Local Functions
* \brief Local functions used by the SWMM output API functions 
* \ingroup Output_Error_Warning_Local_Functions
* \{
*/

/*!
* \brief Validates the SWMM binary output file
* \param[in] p_data Pointer to data_t structure
* \return Error code
*/
int validateFile(data_t *p_data) {
    INT4 magic1, magic2, errcode;
    int  errorcode = 0;

    // --- fast forward to end and read epilogue
    _fseek(p_data->file, -6 * RECORDSIZE, SEEK_END);
    fread(&(p_data->IDPos), RECORDSIZE, 1, p_data->file);
    fread(&(p_data->ObjPropPos), RECORDSIZE, 1, p_data->file);
    fread(&(p_data->ResultsPos), RECORDSIZE, 1, p_data->file);
    fread(&(p_data->Nperiods), RECORDSIZE, 1, p_data->file);
    fread(&errcode, RECORDSIZE, 1, p_data->file);
    fread(&magic2, RECORDSIZE, 1, p_data->file);

    // --- rewind and read magic number from beginning of the file
    _fseek(p_data->file, 0L, SEEK_SET);
    fread(&magic1, RECORDSIZE, 1, p_data->file);

    // Is this a valid SWMM binary output file?
    if (magic1 != magic2)
        errorcode = 435;
    // Does the binary file contain results?
    else if (p_data->Nperiods <= 0)
        errorcode = 436;
    // Were there problems with the model run?
    else if (errcode != 0)
        errorcode = 10;

    return errorcode;
}

/*!
* \brief Initializes the element names array in the data_t structure
* \param[in] p_data Pointer to data_t structure
*/
void initElementNames(data_t *p_data) {
    int j, numNames;

    numNames =
        p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks + p_data->Npolluts;

    // allocate memory for array of idEntries
    p_data->elementNames = (idEntry*)calloc(numNames, sizeof(idEntry));

    // Position the file to the start of the ID entries
    _fseek(p_data->file, p_data->IDPos, SEEK_SET);

    for (j = 0; j < numNames; j++) {
        fread(&(p_data->elementNames[j].length), RECORDSIZE, 1, p_data->file);

        p_data->elementNames[j].IDname =
            (char*)calloc(p_data->elementNames[j].length + 1, sizeof(char));

        fread(p_data->elementNames[j].IDname, sizeof(char),
              p_data->elementNames[j].length, p_data->file);
    }
}

/*!
* \brief Gets the value of a particular time index
* \param[in] p_data Pointer to data_t structure
* \param[in] timeIndex Time index
* \return Time value at the given indexes
*/
double getTimeValue(data_t *p_data, int timeIndex) {

    F_OFF  offset;
    double value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod;

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE * 2, 1, p_data->file);

    return value;
}

/*!
* \brief Gets the value of a particular subcatchment attribute at a given time index 
* \param[in] p_data Pointer to data_t structure
* \param[in] timeIndex Time index
* \param[in] subcatchIndex Subcatchment index
* \param[in] attr Subcatchment attribute
* \return Subcatchment value at the given indexes
*/
float getSubcatchValue(data_t *p_data, int timeIndex, int subcatchIndex,
    SMO_subcatchAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod +
             2 * RECORDSIZE;
    // offset for subcatch
    offset += RECORDSIZE * (subcatchIndex * p_data->SubcatchVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/*!
* \brief Gets the value of a particular node attribute at a given time index
* \param[in] p_data Pointer to data_t structure
* \param[in] timeIndex Time index
* \param[in] nodeIndex Node index
* \param[in] attr Node attribute 
* \return Node value at the given indexes
*/
float getNodeValue(data_t *p_data, int timeIndex, int nodeIndex,
    SMO_nodeAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod +
             2 * RECORDSIZE;
    // offset for node
    offset += RECORDSIZE * (p_data->Nsubcatch * p_data->SubcatchVars +
                            nodeIndex * p_data->NodeVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/*!
* \brief Gets the value of a particular link attribute at a given time index
* \param[in] p_data Pointer to data_t structure
* \param[in] timeIndex Time index
* \param[in] linkIndex Link index
* \param[in] attr Link attribute
* \return Link value at the given indexes
*/
float getLinkValue(data_t *p_data, int timeIndex, int linkIndex,
    SMO_linkAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod +
             2 * RECORDSIZE;
    // offset for link
    offset += RECORDSIZE * (p_data->Nsubcatch * p_data->SubcatchVars +
                            p_data->Nnodes * p_data->NodeVars +
                            linkIndex * p_data->LinkVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/*!
* \brief Gets the value of a particular system attribute at a given time index
* \param[in] p_data Pointer to data_t structure
* \param[in] timeIndex Time index
* \param[in] attr System attribute
* \return System value at the given indexes
*/
float getSystemValue(data_t *p_data, int timeIndex, SMO_systemAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod +
             2 * RECORDSIZE;
    //  offset for system
    offset += RECORDSIZE * (p_data->Nsubcatch * p_data->SubcatchVars +
                            p_data->Nnodes * p_data->NodeVars +
                            p_data->Nlinks * p_data->LinkVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/*!
* \brief Wrapper function for opening file to account for different fopen functions on different platforms
* \param[in] f Pointer to file pointer
* \param[in] name File name 
* \param[in] mode File mode 
* \return Error code
* \note fopen_s is part of C++11 standard
*/
int _fopen(FILE **f, const char *name, const char *mode) {
    int ret = 0;
#ifdef _MSC_VER
    ret = (int)fopen_s(f, name, mode);
#else
    *f = fopen(name, mode);
    if (!*f)
        ret = -1;
#endif
    return ret;
}

/*!
* \brief Wrapper function for fseek to account for different fseek functions on different platforms
* \param[in] stream Pointer to file stream
* \param[in] offset Offset value
* \param[in] whence Whence value
* \return Error code
*/
int _fseek(FILE *stream, F_OFF offset, int whence)
{
#ifdef _MSC_VER
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif

    return FSEEK64(stream, offset, whence);
}

/*!
* \brief Wrapper function for ftell to account for different ftell functions on different platforms
* \param[in] stream Pointer to file stream
* \return Error code
*/
F_OFF _ftell(FILE *stream)
{
#ifdef _MSC_VER
#define FTELL64 _ftelli64
#else
#define FTELL64 ftello
#endif
    return FTELL64(stream);
}

/*!
* \brief Creates a new float array of size n
* \param[in] n Size of the array
* \return Pointer to the float array
*/
float *newFloatArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (float *)malloc((n) * sizeof(float));
}

/*!
* \brief Creates a new int array of size n
* \param[in] n Size of the array
* \return Pointer to the int array
*/
int *newIntArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (int *)malloc((n) * sizeof(int));
}

/*!
* \brief Creates a new char array of size n
* \param[in] n Size of the array
* \return Pointer to the char array
*/
char *newCharArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (char *)malloc((n) * sizeof(char));
}

/*!
* \}
*/
