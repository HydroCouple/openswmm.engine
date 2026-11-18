/**
 * @file      swmm_output.c
 * @brief     Source code providing an API for reading SWMM binary output files.
 * @author    Colleen Barr (US EPA - ORD/NHEERL)
 * @author    Michael E. Tryby (US EPA - ORD/NHEERL) (Modified)
 * @author    Bryant McDonnell (Modified)
 * @author    Caleb Buahin  (US EPA ORD/CESER/WID) (Modified)
 * @date      Created: 2017-08-25
 * @date      Last edited: 2024-10-17
 * @todo Add support for reading results even if the simulation has not completed.
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errormanager.h"
#include "openswmm_output.h"

// NOTE: These depend on machine data model and may change when porting
#ifdef _WIN32 // Windows (32-bit and 64-bit)
/*!
 * \def F_OFF
 * \brief 8 byte / 64 bit integer for large file support on Windows (32-bit and 64-bit)
 * \note This is a Microsoft specific type.
 */
#define F_OFF __int64
#else
#include <sys/types.h>
#include <unistd.h>
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
 * \def DATESIZE
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
    int length;
};

/*!
 * \typedef idEntry
 */
typedef struct IDentry idEntry;

/*!
 * \struct data
 * \brief Structure for SWMM binary output file data
 */
typedef struct {
    /*! \brief File path/name */
    char name[MAXFILENAME + 1];

    /*! \brief File pointer */
    FILE* file;

    /*! \brief Array of pointers to element names */
    struct IDentry* elementNames;

    /*! \brief Number of reporting periods */
    long Nperiods;

    /*! \brief Flow units code */
    int FlowUnits;

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

    /*! \brief Indexes/enum codes of node reporting variables */
    int* SubcatchVarIndexes;

    /*! \brief Number of subcatchment properties */
    int SubcatchProperties;

    /*! \brief Indexes/enum codes of subcatchment properties */
    int* SubcatchPropertyIndexes;

    /*! \brief Number of node reporting variables */
    int NodeVars;

    /*! \brief Indexes/enum codes of node reporting variables */
    int* NodeVarIndexes;

    /*! \brief Number of node properties */
    int NodeProperties;

    /*! \brief Indexes/enum codes of node properties */
    int* NodePropertyIndexes;

    /*! \brief Number of link reporting variables */
    int LinkVars;

    /*! \brief Indexes/enum codes of link reporting variables */
    int* LinkVarIndexes;

    /*! \brief Number of link properties */
    int LinkProperties;

    /*! \brief Indexes/enum codes of link properties */
    int* LinkPropertyIndexes;

    /*! \brief Number of system reporting variables */
    int SysVars;

    /*! \brief Indexes/enum codes of system reporting variables */
    int* SysVarIndexes;

    /*! \brief Start date of simulation */
    double StartDate;

    /*! \brief Reporting time step (seconds) */
    int ReportStep;

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

/**
 * @brief Copy an output-API lookup message into a fixed-size destination.
 *
 * Copies at most the fixed message width without changing a handle. The lookup table supplies
 * bounded literal messages; caller storage is borrowed and not retained.
 *
 * @param[in] errcode Output warning/error code; unrecognized values produce an empty string.
 * @param[out] errmsg Non-null writable ERR_MAXMSG-byte destination (256 bytes).
 * @param[in] length Compatibility capacity argument, ignored; actual writable capacity must still
 *     be ERR_MAXMSG.
 */
void errorLookup(int errcode, char* errmsg, int length);

/**
 * @brief Read the output epilogue and validate its matching magic numbers/results count.
 *
 * Seeks/reads change the stream position but are not checked. A structurally readable completed
 * file is required; this is not a complete corrupt/truncated-file validator. Does not compare the
 * magic against a separate expected constant.
 *
 * @param[in,out] p_data Non-null initialized metadata with a readable FILE stream; receives
 *     ID/property/result offsets and Nperiods.
 * @return ERR435 when leading/trailing magic values differ, ERR436 when no periods exist, WARN10
 *     when the saved run error is nonzero, otherwise zero.
 */
int validateFile(data_t* p_data);

/**
 * @brief Allocate and load the concatenated subcatchment/node/link/pollutant ID table.
 *
 * Allocates the table and each zero-terminated ID string; the handle owns them until SMO_close.
 * Seeks and allocation/read outcomes are unchecked. Calling again without releasing the old table
 * leaks it; callers normally invoke this lazily only when elementNames is null.
 *
 * @param[in,out] p_data Non-null opened metadata with valid counts/IDPos and no previously
 *     allocated elementNames table.
 */
void initElementNames(data_t* p_data);

/**
 * @brief Read one saved absolute simulation date from its period record.
 *
 * Uses the byte stride and result start stored in metadata. Seek/read errors are ignored; caller
 * must provide a valid completed readable file and serialize stream access.
 *
 * @param[in,out] p_data Non-null opened reader; metadata is read, FILE position changes.
 * @param[in] timeIndex Unchecked zero-based saved period index within Nperiods.
 * @return File-stored double date in legacy SWMM decimal days; an unsuccessful read can leave the
 *     result indeterminate.
 */
double getTimeValue(data_t* p_data, int timeIndex);

/**
 * @brief Read a single subcatchment attribute by direct binary-record offset.
 *
 * No bounds, seek or read validation is performed. Requires a well-formed readable completed file
 * and exclusive stream access during the call.
 *
 * @param[in,out] p_data Non-null opened reader with valid offsets/counts; FILE position changes.
 * @param[in] timeIndex Unchecked zero-based saved period below Nperiods.
 * @param[in] subcatchIndex Unchecked zero-based subcatchment index strictly below its count.
 * @param[in] attr Unchecked stored subcatchment variable position, including pollutant slots; not
 *     translated through saved variable codes.
 * @return File-stored float in its original attribute units; an incomplete read can leave the value
 *     indeterminate.
 */
float getSubcatchValue(data_t* p_data, int timeIndex, int subcatchIndex,
                       SMO_subcatchAttribute attr);

/**
 * @brief Read a single node attribute by direct binary-record offset.
 *
 * No bounds, seek or read validation is performed. Requires a well-formed readable completed file
 * and exclusive stream access during the call.
 *
 * @param[in,out] p_data Non-null opened reader with valid offsets/counts; FILE position changes.
 * @param[in] timeIndex Unchecked zero-based saved period below Nperiods.
 * @param[in] nodeIndex Unchecked zero-based node index strictly below its count.
 * @param[in] attr Unchecked stored node variable position, including pollutant slots; not
 *     translated through saved variable codes.
 * @return File-stored float in its original attribute units; an incomplete read can leave the value
 *     indeterminate.
 */
float getNodeValue(data_t* p_data, int timeIndex, int nodeIndex, SMO_nodeAttribute attr);

/**
 * @brief Read a single link attribute by direct binary-record offset.
 *
 * No bounds, seek or read validation is performed. Requires a well-formed readable completed file
 * and exclusive stream access during the call.
 *
 * @param[in,out] p_data Non-null opened reader with valid offsets/counts; FILE position changes.
 * @param[in] timeIndex Unchecked zero-based saved period below Nperiods.
 * @param[in] linkIndex Unchecked zero-based link index strictly below its count.
 * @param[in] attr Unchecked stored link variable position, including pollutant slots; not
 *     translated through saved variable codes.
 * @return File-stored float in its original attribute units; an incomplete read can leave the value
 *     indeterminate.
 */
float getLinkValue(data_t* p_data, int timeIndex, int linkIndex, SMO_linkAttribute attr);

/**
 * @brief Read a single system attribute by direct binary-record offset.
 *
 * No bounds, seek or read validation is performed. Requires a well-formed readable completed file
 * and exclusive stream access during the call.
 *
 * @param[in,out] p_data Non-null opened reader with valid offsets/counts; FILE position changes.
 * @param[in] timeIndex Unchecked zero-based saved period below Nperiods.
 * @param[in] attr Unchecked stored system variable position, including pollutant slots; not
 *     translated through saved variable codes.
 * @return File-stored float in its original attribute units; an incomplete read can leave the value
 *     indeterminate.
 */
float getSystemValue(data_t* p_data, int timeIndex, SMO_systemAttribute attr);

/**
 * @brief Open a C stream through the platform-specific fopen interface.
 *
 * The caller owns any returned stream and must fclose it; an existing stream stored in *f is
 * overwritten without closing.
 *
 * @param[out] f Non-null writable stream pointer; receives the new stream or null.
 * @param[in] name Non-null null-terminated path, borrowed for the call.
 * @param[in] mode Non-null C stdio mode string, borrowed for the call.
 * @return Zero on success; Microsoft fopen_s error on MSVC, otherwise -1 if fopen returned null.
 */
int _fopen(FILE** f, const char* name, const char* mode);

/**
 * @brief Seek a stream using the platform large-file offset implementation.
 *
 * @param[in,out] stream Non-null open stream whose position is changed.
 * @param[in] offset Signed byte offset relative to whence.
 * @param[in] whence SEEK_SET, SEEK_CUR or SEEK_END.
 * @return Underlying _fseeki64/fseeko status: zero on success, nonzero on failure. No output-API
 *     handle error is recorded.
 */
int _fseek(FILE* stream, F_OFF offset, int whence);

/**
 * @brief Read the current stream byte position using large-file offsets.
 *
 * @param[in] stream Non-null open stream.
 * @return Underlying _ftelli64/ftello position, or -1 on failure; no handle error is recorded.
 */
F_OFF _ftell(FILE* stream);

/**
 * @brief Allocate uninitialized float array storage.
 *
 * Caller releases with free or SMO_free. Negative counts convert to an unsigned allocation size;
 * callers must prevent them.
 *
 * @param[in] n Nonnegative element count; no sign or multiplication-overflow checks are performed.
 * @return malloc-owned storage for n elements, or null on failure. For n == 0, follows
 *     implementation-defined malloc(0) behavior.
 */
float* newFloatArray(int n);

/**
 * @brief Allocate uninitialized int array storage.
 *
 * Caller releases with free or SMO_free. Negative counts convert to an unsigned allocation size;
 * callers must prevent them.
 *
 * @param[in] n Nonnegative element count; no sign or multiplication-overflow checks are performed.
 * @return malloc-owned storage for n elements, or null on failure. For n == 0, follows
 *     implementation-defined malloc(0) behavior.
 */
int* newIntArray(int n);

/**
 * @brief Allocate uninitialized char array storage.
 *
 * Caller releases with free or SMO_free. Negative counts convert to an unsigned allocation size;
 * callers must prevent them.
 *
 * @param[in] n Nonnegative element count; no sign or multiplication-overflow checks are performed.
 * @return malloc-owned storage for n elements, or null on failure. For n == 0, follows
 *     implementation-defined malloc(0) behavior.
 */
char* newCharArray(int n);

/**
 * @see Declaration contract for SMO_init().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_init(SMO_Handle* p_handle) {
    int errorcode = 0;
    data_t* priv_data = NULL;

    // // Allocate memory for private data
    priv_data = (data_t*)calloc(1, sizeof(data_t));

    if (priv_data != NULL) {

        priv_data->elementNames = NULL;
        priv_data->file = NULL;
        priv_data->SubcatchVarIndexes = NULL;
        priv_data->NodeVarIndexes = NULL;
        priv_data->LinkVarIndexes = NULL;
        priv_data->SysVarIndexes = NULL;
        priv_data->SubcatchPropertyIndexes = NULL;
        priv_data->NodePropertyIndexes = NULL;
        priv_data->LinkPropertyIndexes = NULL;

        priv_data->error_handle = new_error_manager(errorLookup);

        *p_handle = priv_data;
    } else
        errorcode = -1;

    // TODO: Need to handle errors during initialization better.
    return errorcode;
}

/**
 * @see Declaration contract for SMO_open().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_open(SMO_Handle p_handle, const char* path) {
    int err, errorcode = 0;
    F_OFF offset;

    data_t* p_data;

    p_data = (data_t*)p_handle;

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
            errorcode = ERR434;

        // --- validate the output file
        else if ((err = validateFile(p_data)) != 0)
            errorcode = err;

        // If a warning is encountered read file header
        if (errorcode < 400) {
            // --- otherwise read additional parameters from start of file
            _fseek(p_data->file, 3 * RECORDSIZE, SEEK_SET);
            fread(&(p_data->Nsubcatch), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Nnodes), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Nlinks), RECORDSIZE, 1, p_data->file);
            fread(&(p_data->Npolluts), RECORDSIZE, 1, p_data->file);

            // Read number properties for each object type
            _fseek(p_data->file, p_data->ObjPropPos, SEEK_SET);
            fread(&(p_data->SubcatchProperties), RECORDSIZE, 1, p_data->file);
            if (p_data->SubcatchProperties > 0) {
                if (p_data->SubcatchPropertyIndexes != NULL)
                    free(p_data->SubcatchPropertyIndexes);

                p_data->SubcatchPropertyIndexes = newIntArray(p_data->SubcatchProperties);
                fread(p_data->SubcatchPropertyIndexes, RECORDSIZE, p_data->SubcatchProperties,
                      p_data->file);
            }

            offset =
                ftell(p_data->file) + p_data->Nsubcatch * p_data->SubcatchProperties * RECORDSIZE;
            _fseek(p_data->file, offset, SEEK_SET);

            fread(&(p_data->NodeProperties), RECORDSIZE, 1, p_data->file);
            if (p_data->NodeProperties > 0) {
                if (p_data->NodePropertyIndexes != NULL)
                    free(p_data->NodePropertyIndexes);

                p_data->NodePropertyIndexes = newIntArray(p_data->NodeProperties);
                fread(p_data->NodePropertyIndexes, RECORDSIZE, p_data->NodeProperties,
                      p_data->file);
            }

            offset = ftell(p_data->file) + p_data->Nnodes * p_data->NodeProperties * RECORDSIZE;
            _fseek(p_data->file, offset, SEEK_SET);

            fread(&(p_data->LinkProperties), RECORDSIZE, 1, p_data->file);
            if (p_data->LinkProperties > 0) {
                if (p_data->LinkPropertyIndexes != NULL)
                    free(p_data->LinkPropertyIndexes);

                p_data->LinkPropertyIndexes = newIntArray(p_data->LinkProperties);
                fread(p_data->LinkPropertyIndexes, RECORDSIZE, p_data->LinkProperties,
                      p_data->file);
            }

            offset = ftell(p_data->file) + p_data->Nlinks * p_data->LinkProperties * RECORDSIZE;
            _fseek(p_data->file, offset, SEEK_SET);

            // Read number & codes of computed variables
            fread(&(p_data->SubcatchVars), RECORDSIZE, 1, p_data->file); // # Subcatch variables
            if (p_data->SubcatchVars > 0) {
                if (p_data->SubcatchVarIndexes != NULL)
                    free(p_data->SubcatchVarIndexes);

                p_data->SubcatchVarIndexes = newIntArray(p_data->SubcatchVars);
                fread(p_data->SubcatchVarIndexes, RECORDSIZE, p_data->SubcatchVars, p_data->file);
            }

            fread(&(p_data->NodeVars), RECORDSIZE, 1, p_data->file); // # Node variables
            if (p_data->NodeVars > 0) {
                if (p_data->NodeVarIndexes != NULL)
                    free(p_data->NodeVarIndexes);

                p_data->NodeVarIndexes = newIntArray(p_data->NodeVars);
                fread(p_data->NodeVarIndexes, RECORDSIZE, p_data->NodeVars, p_data->file);
            }

            fread(&(p_data->LinkVars), RECORDSIZE, 1, p_data->file); // # Link variables
            if (p_data->LinkVars > 0) {
                if (p_data->LinkVarIndexes != NULL)
                    free(p_data->LinkVarIndexes);

                p_data->LinkVarIndexes = newIntArray(p_data->LinkVars);
                fread(p_data->LinkVarIndexes, RECORDSIZE, p_data->LinkVars, p_data->file);
            }

            fread(&(p_data->SysVars), RECORDSIZE, 1, p_data->file); // # System variables
            if (p_data->SysVars > 0) {
                if (p_data->SysVarIndexes != NULL)
                    free(p_data->SysVarIndexes);

                p_data->SysVarIndexes = newIntArray(p_data->SysVars);
                fread(p_data->SysVarIndexes, RECORDSIZE, p_data->SysVars, p_data->file);
            }

            // --- read data just before start of output results
            offset = p_data->ResultsPos - 3 * RECORDSIZE;
            _fseek(p_data->file, offset, SEEK_SET);
            fread(&(p_data->StartDate), DATESIZE, 1, p_data->file);
            fread(&(p_data->ReportStep), RECORDSIZE, 1, p_data->file);

            // --- compute number of bytes of results values used per time
            // period
            p_data->BytesPerPeriod =
                DATESIZE +
                (p_data->Nsubcatch * p_data->SubcatchVars + p_data->Nnodes * p_data->NodeVars +
                 p_data->Nlinks * p_data->LinkVars + p_data->SysVars) *
                    RECORDSIZE;
        }
    }
    // If error close the binary file (but don't free the handle --
    // the caller owns it and will call SMO_close)
    if (errorcode > 400) {
        set_error(p_data->error_handle, errorcode);
        if (p_data->file != NULL) {
            fclose(p_data->file);
            p_data->file = NULL;
        }
    }

    return errorcode;
}

/**
 * @see Declaration contract for SMO_close().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_close(SMO_Handle* p_handle) {
    data_t* p_data;
    int i, n, errorcode = 0;

    p_data = (data_t*)*p_handle;

    if (p_data == NULL)
        errorcode = -1;

    else {
        if (p_data->elementNames != NULL) {
            n = p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks + p_data->Npolluts;

            for (i = 0; i < n; i++)
                free(p_data->elementNames[i].IDname);

            free(p_data->elementNames);
            p_data->elementNames = NULL;
        }

        if (p_data->SubcatchVarIndexes != NULL) {
            free(p_data->SubcatchVarIndexes);
            p_data->SubcatchVarIndexes = NULL;
        }

        if (p_data->NodeVarIndexes != NULL) {
            free(p_data->NodeVarIndexes);
            p_data->NodeVarIndexes = NULL;
        }

        if (p_data->LinkVarIndexes != NULL) {
            free(p_data->LinkVarIndexes);
            p_data->LinkVarIndexes = NULL;
        }

        if (p_data->SysVarIndexes != NULL) {
            free(p_data->SysVarIndexes);
            p_data->SysVarIndexes = NULL;
        }

        if (p_data->SubcatchPropertyIndexes != NULL) {
            free(p_data->SubcatchPropertyIndexes);
            p_data->SubcatchPropertyIndexes = NULL;
        }

        if (p_data->NodePropertyIndexes != NULL) {
            free(p_data->NodePropertyIndexes);
            p_data->NodePropertyIndexes = NULL;
        }

        if (p_data->LinkPropertyIndexes != NULL) {
            free(p_data->LinkPropertyIndexes);
            p_data->LinkPropertyIndexes = NULL;
        }

        dst_error_manager(p_data->error_handle);

        if (p_data->file != NULL) {
            fclose(p_data->file);
            p_data->file = NULL;
        }

        free(p_data);

        *p_handle = NULL;
    }

    return errorcode;
}

/**
 * @see Declaration contract for SMO_getVersion().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVersion(SMO_Handle p_handle, int* version) {
    int errorcode = 0;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        return -1;
    else {
        fseek(p_data->file, 1 * RECORDSIZE, SEEK_SET);
        if (fread(version, RECORDSIZE, 1, p_data->file) != 1)
            errorcode = ERR436;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getProjectSize().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getProjectSize(SMO_Handle p_handle, int** elementCount,
                                                      int* length) {
    int errorcode = 0;
    int* temp;
    data_t* p_data;

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
        temp[3] = 1; // NSystems
        temp[4] = p_data->Npolluts;

        *elementCount = temp;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getUnits().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getUnits(SMO_Handle p_handle, int** unitFlag, int* length) {
    int errorcode = 0;
    int* temp;
    F_OFF offset;
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
        fseek(p_data->file, 2 * RECORDSIZE, SEEK_SET);
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

/**
 * @see Declaration contract for SMO_getFlowUnits().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getFlowUnits(SMO_Handle p_handle, int* unitFlag) {
    int errorcode = 0;
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

/**
 * @see Declaration contract for SMO_getPollutantUnits().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPollutantUnits(SMO_Handle p_handle, int** unitFlag,
                                                         int* length) {
    int errorcode = 0;
    int* temp;
    F_OFF offset;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (MEMCHECK(temp = newIntArray(p_data->Npolluts)))
        errorcode = 414;
    else {
        offset = p_data->ObjPropPos - (p_data->Npolluts * RECORDSIZE);
        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->Npolluts, p_data->file);

        *unitFlag = temp;
        *length = p_data->Npolluts;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getStartDate().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getStartDate(SMO_Handle p_handle, double* date) {
    int errorcode = 0;
    data_t* p_data;

    *date = -1.0;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        *date = p_data->StartDate;

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getTimes().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getTimes(SMO_Handle p_handle, SMO_time code, int* time) {
    int errorcode = 0;
    data_t* p_data;

    *time = -1;

    p_data = (data_t*)p_handle;

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
            errorcode = ERR421;
        }
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getElementName().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getElementName(SMO_Handle p_handle, SMO_elementType type,
                                                      int index, char** name, int* length) {
    int idx = -1, errorcode = 0;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = 410;
    else if (p_data->file == NULL)
        errorcode = ERR411;
    else {
        // Initialize the name array if necessary
        if (p_data->elementNames == NULL)
            initElementNames(p_data);

        switch (type) {
        case SMO_subcatch:
            if (index < 0 || index >= p_data->Nsubcatch)
                errorcode = ERR423;
            else
                idx = index;
            break;

        case SMO_node:
            if (index < 0 || index >= p_data->Nnodes)
                errorcode = ERR423;
            else
                idx = p_data->Nsubcatch + index;
            break;

        case SMO_link:
            if (index < 0 || index >= p_data->Nlinks)
                errorcode = ERR423;
            else
                idx = p_data->Nsubcatch + p_data->Nnodes + index;
            break;

        case SMO_pollut:
            if (index < 0 || index >= p_data->Npolluts)
                errorcode = ERR423;
            else
                idx = p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks + index;
            break;

        default:
            errorcode = ERR421;
        }

        if (!errorcode) {
            *length = p_data->elementNames[idx].length;
            *name = newCharArray(*length + 1);
            // Writes IDname and an additional null character to name

#ifdef _MSC_VER
            strncpy_s(*name, (*length + 1) * sizeof(char), p_data->elementNames[idx].IDname,
                      (*length + 1) * sizeof(char));
#else
            strncpy(*name, p_data->elementNames[idx].IDname, (*length + 1) * sizeof(char));
#endif
        }
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getNumVars().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNumVars(SMO_Handle p_handle, SMO_elementType type,
                                                  int* count) {
    int errorcode = 0;
    data_t* p_data;

    *count = -1;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            *count = p_data->SubcatchVars;
            break;
        case SMO_node:
            *count = p_data->NodeVars;
            break;
        case SMO_link:
            *count = p_data->LinkVars;
            break;
        case SMO_sys:
            *count = p_data->SysVars;
            break;
        default:
            errorcode = ERR421;
        }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getVarCode().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVarCode(SMO_Handle p_handle, SMO_elementType type,
                                                  int varIndex, int* varCode) {
    int errorcode = 0;
    int* temp = NULL;
    data_t* p_data = NULL;
    *varCode = -1;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            if (varIndex < 0 || varIndex >= p_data->SubcatchVars)
                errorcode = ERR423;
            else
                temp = p_data->SubcatchVarIndexes;
            break;
        case SMO_node:
            if (varIndex < 0 || varIndex >= p_data->NodeVars)
                errorcode = ERR423;
            else
                temp = p_data->NodeVarIndexes;
            break;
        case SMO_link:
            if (varIndex < 0 || varIndex >= p_data->LinkVars)
                errorcode = ERR423;
            else
                temp = p_data->LinkVarIndexes;
            break;
        case SMO_sys:
            if (varIndex < 0 || varIndex >= p_data->SysVars)
                errorcode = ERR423;
            else
                temp = p_data->SysVarIndexes;
            break;
        default:
            errorcode = ERR421;
        }

    if (!errorcode)
        *varCode = temp[varIndex];

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getVarCodes().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getVarCodes(SMO_Handle p_handle, SMO_elementType type,
                                                   int** varCodes, int* size) {
    int errorcode = 0;
    int* temp = NULL;
    data_t* p_data = NULL;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            *size = p_data->SubcatchVars;
            temp = p_data->SubcatchVarIndexes;
            break;
        case SMO_node:
            *size = p_data->NodeVars;
            temp = p_data->NodeVarIndexes;
            break;
        case SMO_link:
            *size = p_data->LinkVars;
            temp = p_data->LinkVarIndexes;
            break;
        case SMO_sys:
            *size = p_data->SysVars;
            temp = p_data->SysVarIndexes;
            break;
        default:
            errorcode = ERR421;
        }

    if (!errorcode) {
        *varCodes = newIntArray(*size);
        for (int i = 0; i < *size; i++)
            (*varCodes)[i] = temp[i];
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getNumProperties().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNumProperties(SMO_Handle p_handle, SMO_elementType type,
                                                        int* count) {
    int errorcode = 0;
    data_t* p_data;

    *count = -1;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            *count = p_data->SubcatchProperties;
            break;
        case SMO_node:
            *count = p_data->NodeProperties;
            break;
        case SMO_link:
            *count = p_data->LinkProperties;
            break;
        default:
            errorcode = ERR421;
        }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getPropertyCode().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyCode(SMO_Handle p_handle, SMO_elementType type,
                                                       int propertyIndex, int* propertyCode) {
    int errorcode = 0;
    int* temp = NULL;
    data_t* p_data = NULL;
    *propertyCode = -1;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            if (propertyIndex < 0 || propertyIndex >= p_data->SubcatchProperties)
                errorcode = ERR423;
            else
                temp = p_data->SubcatchPropertyIndexes;
            break;
        case SMO_node:
            if (propertyIndex < 0 || propertyIndex >= p_data->NodeProperties)
                errorcode = ERR423;
            else
                temp = p_data->NodePropertyIndexes;
            break;
        case SMO_link:
            if (propertyIndex < 0 || propertyIndex >= p_data->LinkProperties)
                errorcode = ERR423;
            else
                temp = p_data->LinkPropertyIndexes;
            break;
        default:
            errorcode = ERR421;
        }

    if (!errorcode) {
        *propertyCode = temp[propertyIndex];
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getPropertyCodes().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyCodes(SMO_Handle p_handle, SMO_elementType type,
                                                        int** propertyCodes, int* size) {
    int errorcode = 0;
    int* temp = NULL;
    data_t* p_data = NULL;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else
        switch (type) {
        case SMO_subcatch:
            *size = p_data->SubcatchProperties;
            temp = p_data->SubcatchPropertyIndexes;
            break;
        case SMO_node:
            *size = p_data->NodeProperties;
            temp = p_data->NodePropertyIndexes;
            break;
        case SMO_link:
            *size = p_data->LinkProperties;
            temp = p_data->LinkPropertyIndexes;
            break;
        default:
            errorcode = ERR421;
        }

    if (!errorcode) {
        *propertyCodes = newIntArray(*size);
        for (int i = 0; i < *size; i++)
            (*propertyCodes)[i] = temp[i];
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSubcatchSeries().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchSeries(SMO_Handle p_handle, int subcatchIndex,
                                                         SMO_subcatchAttribute attr,
                                                         int startPeriod, int endPeriod,
                                                         float** outValueArray, int* length) {
    int k, len, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (subcatchIndex < 0 || subcatchIndex > p_data->Nsubcatch)
        errorcode = ERR423;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods || endPeriod <= startPeriod)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(len = endPeriod - startPeriod))
        errorcode = ERR411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getSubcatchValue(p_data, startPeriod + k, subcatchIndex, attr);

        *outValueArray = temp;
        *length = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getPropertyValue().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyValue(SMO_Handle p_handle, SMO_elementType type,
                                                        int propertyIndex, int elementIndex,
                                                        float* value) {
    int errorcode = 0;
    F_OFF offset;

    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else {
        offset = p_data->ObjPropPos;

        switch (type) {
        case SMO_subcatch:
            if (propertyIndex < 0 || propertyIndex >= p_data->SubcatchProperties)
                errorcode = ERR423;
            else if (elementIndex < 0 || elementIndex >= p_data->Nsubcatch)
                errorcode = ERR423;
            else {
                offset += (1 + p_data->SubcatchProperties +
                           elementIndex * p_data->SubcatchProperties + propertyIndex) *
                          RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(value, RECORDSIZE, 1, p_data->file);
            }
            break;
        case SMO_node:
            if (propertyIndex < 0 || propertyIndex >= p_data->NodeProperties)
                errorcode = ERR423;
            else if (elementIndex < 0 || elementIndex >= p_data->Nnodes)
                errorcode = ERR423;
            else {
                offset += (1 + p_data->SubcatchProperties +
                           p_data->Nsubcatch * p_data->SubcatchProperties) *
                          RECORDSIZE;
                offset += (1 + p_data->NodeProperties + elementIndex * p_data->NodeProperties +
                           propertyIndex) *
                          RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(value, RECORDSIZE, 1, p_data->file);
            }
            break;

        case SMO_link:
            if (propertyIndex < 0 || propertyIndex >= p_data->LinkProperties)
                errorcode = ERR423;
            else if (elementIndex < 0 || elementIndex >= p_data->Nlinks)
                errorcode = ERR423;
            else {
                offset += (1 + p_data->SubcatchProperties +
                           p_data->Nsubcatch * p_data->SubcatchProperties) *
                          RECORDSIZE;
                offset += (1 + p_data->NodeProperties + p_data->Nnodes * p_data->NodeProperties) *
                          RECORDSIZE;
                offset += (1 + p_data->LinkProperties + elementIndex * p_data->LinkProperties +
                           propertyIndex) *
                          RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(value, RECORDSIZE, 1, p_data->file);
            }
            break;

        default:
            errorcode = ERR421;
        }
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getPropertyValues().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getPropertyValues(SMO_Handle p_handle, SMO_elementType type,
                                                         int elementIndex, float** outValueArray,
                                                         int* length) {
    int errorcode = 0;
    F_OFF offset;

    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else {
        switch (type) {
        case SMO_subcatch:
            if (elementIndex < 0 || elementIndex >= p_data->Nsubcatch)
                errorcode = ERR423;
            else {
                *outValueArray = newFloatArray(p_data->SubcatchProperties);
                *length = p_data->SubcatchProperties;
                offset = p_data->ObjPropPos + (1 + p_data->SubcatchProperties +
                                               elementIndex * p_data->SubcatchProperties) *
                                                  RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(*outValueArray, RECORDSIZE, p_data->SubcatchProperties, p_data->file);
            }
            break;
        case SMO_node:
            if (elementIndex < 0 || elementIndex >= p_data->Nnodes)
                errorcode = ERR423;
            else {
                *outValueArray = newFloatArray(p_data->NodeProperties);
                *length = p_data->NodeProperties;

                offset = p_data->ObjPropPos + (1 + p_data->SubcatchProperties +
                                               p_data->Nsubcatch * p_data->SubcatchProperties) *
                                                  RECORDSIZE;
                offset += (1 + p_data->NodeProperties + elementIndex * p_data->NodeProperties) *
                          RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(*outValueArray, RECORDSIZE, p_data->NodeProperties, p_data->file);
            }
            break;
        case SMO_link:
            if (elementIndex < 0 || elementIndex >= p_data->Nlinks)
                errorcode = ERR423;
            else {
                *outValueArray = newFloatArray(p_data->LinkProperties);
                *length = p_data->LinkProperties;

                offset = p_data->ObjPropPos + (1 + p_data->SubcatchProperties +
                                               p_data->Nsubcatch * p_data->SubcatchProperties) *
                                                  RECORDSIZE;
                offset += (1 + p_data->NodeProperties + p_data->Nnodes * p_data->NodeProperties) *
                          RECORDSIZE;
                offset += (1 + p_data->LinkProperties + elementIndex * p_data->LinkProperties) *
                          RECORDSIZE;
                _fseek(p_data->file, offset, SEEK_SET);
                fread(*outValueArray, RECORDSIZE, p_data->LinkProperties, p_data->file);
            }
            break;
        default:
            errorcode = ERR421;
        }
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getNodeSeries().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeSeries(SMO_Handle p_handle, int nodeIndex,
                                                     SMO_nodeAttribute attr, int startPeriod,
                                                     int endPeriod, float** outValueArray,
                                                     int* length) {
    int k, len, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (nodeIndex < 0 || nodeIndex > p_data->Nnodes)
        errorcode = ERR423;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods || endPeriod <= startPeriod)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(len = endPeriod - startPeriod))
        errorcode = ERR411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getNodeValue(p_data, startPeriod + k, nodeIndex, attr);

        *outValueArray = temp;
        *length = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getLinkSeries().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkSeries(SMO_Handle p_handle, int linkIndex,
                                                     SMO_linkAttribute attr, int startPeriod,
                                                     int endPeriod, float** outValueArray,
                                                     int* length) {
    int k, len, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (linkIndex < 0 || linkIndex > p_data->Nlinks)
        errorcode = ERR423;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods || endPeriod <= startPeriod)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(len = endPeriod - startPeriod))
        errorcode = ERR411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getLinkValue(p_data, startPeriod + k, linkIndex, attr);

        *outValueArray = temp;
        *length = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSystemSeries().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemSeries(SMO_Handle p_handle,
                                                       SMO_systemAttribute attr, int startPeriod,
                                                       int endPeriod, float** outValueArray,
                                                       int* length) {
    int k, len, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (startPeriod < 0 || startPeriod >= p_data->Nperiods || endPeriod <= startPeriod)
        errorcode = ERR423;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(len = endPeriod - startPeriod))
        errorcode = ERR411;
    else {
        // loop over and build time series
        for (k = 0; k < len; k++)
            temp[k] = getSystemValue(p_data, startPeriod + k, attr);

        *outValueArray = temp;
        *length = len;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSubcatchAttribute().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchAttribute(SMO_Handle p_handle, int periodIndex,
                                                            SMO_subcatchAttribute attr,
                                                            float** outValueArray, int* length) {
    int k, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(p_data->Nsubcatch))
        errorcode = ERR411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nsubcatch; k++)
            temp[k] = getSubcatchValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length = p_data->Nsubcatch;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getNodeAttribute().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeAttribute(SMO_Handle p_handle, int periodIndex,
                                                        SMO_nodeAttribute attr,
                                                        float** outValueArray, int* length) {
    int k, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(p_data->Nnodes))
        errorcode = ERR411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nnodes; k++)
            temp[k] = getNodeValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length = p_data->Nnodes;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getLinkAttribute().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkAttribute(SMO_Handle p_handle, int periodIndex,
                                                        SMO_linkAttribute attr,
                                                        float** outValueArray, int* length) {
    int k, errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    // Check memory for outValues
    else if MEMCHECK (temp = newFloatArray(p_data->Nlinks))
        errorcode = ERR411;
    else {
        // loop over and pull result
        for (k = 0; k < p_data->Nlinks; k++)
            temp[k] = getLinkValue(p_data, periodIndex, k, attr);

        *outValueArray = temp;
        *length = p_data->Nlinks;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSystemAttribute().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemAttribute(SMO_Handle p_handle, int periodIndex,
                                                          SMO_systemAttribute attr,
                                                          float** outValueArray, int* length) {
    int errorcode = 0;
    float* temp;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    else if MEMCHECK (temp = newFloatArray(1))
        errorcode = ERR411;
    else {
        // don't need to loop since there's only one system
        temp[0] = getSystemValue(p_data, periodIndex, attr);

        *outValueArray = temp;
        *length = 1;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSubcatchResult().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSubcatchResult(SMO_Handle p_handle, int periodIndex,
                                                         int subcatchIndex, float** outValueArray,
                                                         int* arrayLength) {
    int errorcode = 0;
    float* temp;
    F_OFF offset;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    else if (subcatchIndex < 0 || subcatchIndex > p_data->Nsubcatch)
        errorcode = ERR423;
    else if MEMCHECK (temp = newFloatArray(p_data->SubcatchVars))
        errorcode = ERR411;
    else {
        // --- compute offset into output file
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod + 2 * RECORDSIZE;
        // add offset for subcatchment
        offset += (subcatchIndex * p_data->SubcatchVars) * RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->SubcatchVars, p_data->file);

        *outValueArray = temp;
        *arrayLength = p_data->SubcatchVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getNodeResult().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getNodeResult(SMO_Handle p_handle, int periodIndex,
                                                     int nodeIndex, float** outValueArray,
                                                     int* arrayLength) {
    int errorcode = 0;
    float* temp;
    F_OFF offset;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    else if (nodeIndex < 0 || nodeIndex > p_data->Nnodes)
        errorcode = ERR423;
    else if MEMCHECK (temp = newFloatArray(p_data->NodeVars))
        errorcode = ERR411;
    else {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod + 2 * RECORDSIZE;
        // add offset for subcatchment and node
        offset +=
            (p_data->Nsubcatch * p_data->SubcatchVars + nodeIndex * p_data->NodeVars) * RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->NodeVars, p_data->file);

        *outValueArray = temp;
        *arrayLength = p_data->NodeVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getLinkResult().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getLinkResult(SMO_Handle p_handle, int periodIndex,
                                                     int linkIndex, float** outValueArray,
                                                     int* arrayLength) {
    int errorcode = 0;
    float* temp;
    F_OFF offset;
    data_t* p_data;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = 422;
    else if (linkIndex < 0 || linkIndex > p_data->Nlinks)
        errorcode = ERR423;
    else if MEMCHECK (temp = newFloatArray(p_data->LinkVars))
        errorcode = ERR411;
    else {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod + 2 * RECORDSIZE;
        // add offset for subcatchment and node and link
        offset += (p_data->Nsubcatch * p_data->SubcatchVars + p_data->Nnodes * p_data->NodeVars +
                   linkIndex * p_data->LinkVars) *
                  RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->LinkVars, p_data->file);

        *outValueArray = temp;
        *arrayLength = p_data->LinkVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_getSystemResult().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_getSystemResult(SMO_Handle p_handle, int periodIndex,
                                                       int dummyIndex, float** outValueArray,
                                                       int* arrayLength) {
    (void)dummyIndex;
    int errorcode = 0;
    float* temp = NULL;
    F_OFF offset;
    data_t* p_data = NULL;

    p_data = (data_t*)p_handle;

    if (p_data == NULL)
        errorcode = -1;
    else if (periodIndex < 0 || periodIndex >= p_data->Nperiods)
        errorcode = ERR422;
    else if MEMCHECK (temp = newFloatArray(p_data->SysVars))
        errorcode = ERR411;
    {
        // calculate byte offset to start time for series
        offset = p_data->ResultsPos + (periodIndex)*p_data->BytesPerPeriod + 2 * RECORDSIZE;
        // add offset for subcatchment and node and link (system starts after
        // the last link)
        offset += (p_data->Nsubcatch * p_data->SubcatchVars + p_data->Nnodes * p_data->NodeVars +
                   p_data->Nlinks * p_data->LinkVars) *
                  RECORDSIZE;

        _fseek(p_data->file, offset, SEEK_SET);
        fread(temp, RECORDSIZE, p_data->SysVars, p_data->file);

        *outValueArray = temp;
        *arrayLength = p_data->SysVars;
    }

    return set_error(p_data->error_handle, errorcode);
}

/**
 * @see Declaration contract for SMO_free().
 */
void EXPORT_OPENSWMMCORE_OUTPUT_API SMO_free(void** array) {
    if (array != NULL) {
        free(*array);
        *array = NULL;
    }
}

/**
 * @see Declaration contract for SMO_clearError().
 */
void EXPORT_OPENSWMMCORE_OUTPUT_API SMO_clearError(SMO_Handle p_handle) {
    data_t* p_data;

    p_data = (data_t*)p_handle;
    clear_error(p_data->error_handle);
}

/**
 * @see Declaration contract for SMO_checkError().
 */
int EXPORT_OPENSWMMCORE_OUTPUT_API SMO_checkError(SMO_Handle p_handle, char** msg_buffer) {
    int errorcode = 0;
    char* temp = NULL;
    data_t* p_data;

    p_data = (data_t*)p_handle;

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

/**
 * @see Local declaration contract for errorLookup().
 */
void errorLookup(int errcode, char* dest_msg, int dest_len) {
    (void)dest_len;
    char msg[ERR_MAXMSG];

    switch (errcode) {
#ifdef _MSC_VER
#define ERR(code, string)                                                                          \
    case code:                                                                                     \
        strncpy_s(msg, ERR_MAXMSG, string, ERR_MAXMSG);                                            \
        break;
#include "messages.txt"
#undef ERR
#else
#define ERR(code, string)                                                                          \
    case code:                                                                                     \
        strcpy(msg, string);                                                                       \
        break;
#include "messages.txt"
#undef ERR
#endif
    default:
#ifdef _MSC_VER
        strncpy_s(msg, ERR_MAXMSG, "", ERR_MAXMSG);
#else
        strcpy(msg, "");
#endif
    }

#ifdef _MSC_VER
    strncpy_s(dest_msg, ERR_MAXMSG, msg, ERR_MAXMSG);
#else
    strncpy(dest_msg, msg, ERR_MAXMSG);
#endif
}

/*!
 * \addtogroup SWMM_Output_API_Local_Functions SWMM Output API Local Functions
 * \brief Local functions used by the SWMM output API functions
 * \ingroup Output_Error_Warning_Local_Functions
 * \{
 */

/**
 * @see Local declaration contract for validateFile().
 */
int validateFile(data_t* p_data) {
    INT4 magic1, magic2, errcode;
    int errorcode = 0;

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
        errorcode = ERR435;
    // Does the binary file contain results?
    else if (p_data->Nperiods <= 0)
        errorcode = ERR436;
    // Were there problems with the model run?
    else if (errcode != 0)
        errorcode = WARN10;

    return errorcode;
}

/**
 * @see Local declaration contract for initElementNames().
 */
void initElementNames(data_t* p_data) {
    int j, numNames;

    numNames = p_data->Nsubcatch + p_data->Nnodes + p_data->Nlinks + p_data->Npolluts;

    // allocate memory for array of idEntries
    p_data->elementNames = (idEntry*)calloc(numNames, sizeof(idEntry));

    // Position the file to the start of the ID entries
    _fseek(p_data->file, p_data->IDPos, SEEK_SET);

    for (j = 0; j < numNames; j++) {
        fread(&(p_data->elementNames[j].length), RECORDSIZE, 1, p_data->file);

        p_data->elementNames[j].IDname =
            (char*)calloc(p_data->elementNames[j].length + 1, sizeof(char));

        fread(p_data->elementNames[j].IDname, sizeof(char), p_data->elementNames[j].length,
              p_data->file);
    }
}

/**
 * @see Local declaration contract for getTimeValue().
 */
double getTimeValue(data_t* p_data, int timeIndex) {

    F_OFF offset;
    double value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod;

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE * 2, 1, p_data->file);

    return value;
}

/**
 * @see Local declaration contract for getSubcatchValue().
 */
float getSubcatchValue(data_t* p_data, int timeIndex, int subcatchIndex,
                       SMO_subcatchAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod + 2 * RECORDSIZE;
    // offset for subcatch
    offset += RECORDSIZE * (subcatchIndex * p_data->SubcatchVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/**
 * @see Local declaration contract for getNodeValue().
 */
float getNodeValue(data_t* p_data, int timeIndex, int nodeIndex, SMO_nodeAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod + 2 * RECORDSIZE;
    // offset for node
    offset += RECORDSIZE *
              (p_data->Nsubcatch * p_data->SubcatchVars + nodeIndex * p_data->NodeVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/**
 * @see Local declaration contract for getLinkValue().
 */
float getLinkValue(data_t* p_data, int timeIndex, int linkIndex, SMO_linkAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod + 2 * RECORDSIZE;
    // offset for link
    offset +=
        RECORDSIZE * (p_data->Nsubcatch * p_data->SubcatchVars + p_data->Nnodes * p_data->NodeVars +
                      linkIndex * p_data->LinkVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/**
 * @see Local declaration contract for getSystemValue().
 */
float getSystemValue(data_t* p_data, int timeIndex, SMO_systemAttribute attr) {

    F_OFF offset;
    float value;

    // --- compute offset into output file
    offset = p_data->ResultsPos + timeIndex * p_data->BytesPerPeriod + 2 * RECORDSIZE;
    //  offset for system
    offset +=
        RECORDSIZE * (p_data->Nsubcatch * p_data->SubcatchVars + p_data->Nnodes * p_data->NodeVars +
                      p_data->Nlinks * p_data->LinkVars + attr);

    // --- re-position the file and read the result
    _fseek(p_data->file, offset, SEEK_SET);
    fread(&value, RECORDSIZE, 1, p_data->file);

    return value;
}

/**
 * @see Local declaration contract for _fopen().
 */
int _fopen(FILE** f, const char* name, const char* mode) {
    int ret = 0;
#ifdef _MSC_VER
    ret = (int)fopen_s(f, name, mode);
#else
    *f = fopen(name, mode);
    if (*f == NULL)
        ret = -1;
#endif
    return ret;
}

/**
 * @see Local declaration contract for _fseek().
 */
int _fseek(FILE* stream, F_OFF offset, int whence) {
#ifdef _MSC_VER
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif

    return FSEEK64(stream, offset, whence);
}

/**
 * @see Local declaration contract for _ftell().
 */
F_OFF _ftell(FILE* stream) {
#ifdef _MSC_VER
#define FTELL64 _ftelli64
#else
#define FTELL64 ftello
#endif
    return FTELL64(stream);
}

/**
 * @see Local declaration contract for newFloatArray().
 */
float* newFloatArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (float*)malloc((n) * sizeof(float));
}

/**
 * @see Local declaration contract for newIntArray().
 */
int* newIntArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (int*)malloc((n) * sizeof(int));
}

/**
 * @see Local declaration contract for newCharArray().
 */
char* newCharArray(int n)
//
//  Warning: Caller must free memory allocated by this function.
//
{
    return (char*)malloc((n) * sizeof(char));
}

/*!
 * \}
 */
