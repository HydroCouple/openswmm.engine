/*! \file swmm5.h
 * \brief Prototypes for SWMM5 API functions.
 * \author L. Rossman
 * \date Created on: 2021-11-01
 * \date Last edited on: 2024-12-23 (Build 5.3.0)
 * \details This file contains the prototypes for SWMM5 API functions.
 *
 *  Update History
 *  ==============
 *  - Build 5.3.0:
 *      - Added new functions to support saving hotstart files at specific times.
 *      - Expansions to the SWMM API to include attributes of more objects and water quality.
 */
#ifndef SWMM5_H
#define SWMM5_H

#include "swmm5_export.h"

// --- use "C" linkage for C++ programs
#ifdef __cplusplus
extern "C" { 
#endif

/*!
 * \enum swmm_Object
 * \brief Enumeration of object types used in SWMM5
 */
typedef enum
{
    /*! \brief Rain gages */
    swmm_GAGE,
    /*! \brief Subcatchments */
    swmm_SUBCATCH,
    /*! \brief Nodes */
    swmm_NODE,
    /*! \brief Links */
    swmm_LINK,
    /*! \brief Pollutants */
    swmm_POLLUTANT,
    /*! \brief Land uses */
    swmm_LANDUSE,
    /*! \brief Time patterns */
    swmm_TIME_PATTERN,
    /*! \brief Curve functions */
    swmm_CURVE,
    /*! \brief Time series */
    swmm_TIMESERIES,
    /*! \brief Control rules */
    swmm_CONTROL_RULE,
    /*! \brief Transects */
    swmm_TRANSECT,
    /*! \brief Aquifers */
    swmm_AQUIFER,
    /*! \brief Unit hydrographs */
    swmm_UNIT_HYDROGRAPH,
    /*! \brief Snow packs */
    swmm_SNOWPACK,
    /*! \brief Cross section shape */
    smmm_XSECTION_SHAPE,
    /*! \brief Low impact development units */
    swmm_LID,
    /*! \brief Street*/
    swmm_STREET,
    /*! \brief Inlet */
    swmm_INLET,
    /*! \brief System */
    swmm_SYSTEM = 100
} swmm_Object;

/*!
 * \enum swmm_NodeType
 * \brief Enumeration of node types used in SWMM5
 */
typedef enum
{
    /*! \brief Junction node */
    swmm_JUNCTION = 0,
    /*! \brief Outfall node */
    swmm_OUTFALL = 1,
    /*! \brief Storage node */
    swmm_STORAGE = 2,
    /*! \brief Divider node */
    swmm_DIVIDER = 3
} swmm_NodeType;

/*!
 * \enum swmm_LinkType
 * \brief Enumeration of link types used in SWMM5
 */
typedef enum
{
    /*! \brief Conduit link */
    swmm_CONDUIT = 0,
    /*! \brief Pump link */
    swmm_PUMP = 1,
    /*! \brief Orifice link */
    swmm_ORIFICE = 2,
    /*! \brief Weir link */
    swmm_WEIR = 3,
    /*! \brief Outlet link */
    swmm_OUTLET = 4
} swmm_LinkType;

/*!
 * \enum swmm_GageProperty
 * \brief Enumeration of gage properties used in SWMM5
 */
typedef enum
{
    /*! \brief Total precipitation */
    swmm_GAGE_TOTAL_PRECIPITATION = 100,
    /*! \brief Rainfall */
    swmm_GAGE_RAINFALL = 101,
    /*! \brief Snowfall */
    swmm_GAGE_SNOWFALL = 102,
} swmm_GageProperty;

/*!
 * \enum swmm_SubcatchProperty
 * \brief Enumeration of subcatchment properties used in SWMM5
 */
typedef enum
{
    /*! \brief Area */
    swmm_SUBCATCH_AREA = 200,
    /*! \brief Rain gage */
    swmm_SUBCATCH_RAINGAGE = 201,
    /*! \brief Rainfall */
    swmm_SUBCATCH_RAINFALL = 202,
    /*! \brief Evaporation */
    swmm_SUBCATCH_EVAP = 203,
    /*! \brief Infiltration */
    swmm_SUBCATCH_INFIL = 204,
    /*! \brief Runoff */
    swmm_SUBCATCH_RUNOFF = 205,
    /*! \brief Report flag */
    swmm_SUBCATCH_RPTFLAG = 206,
    /*! \brief Width */
    swmm_SUBCATCH_WIDTH = 207,
    /*! \brief Slope */
    swmm_SUBCATCH_SLOPE = 208,
    /*! \brief Curb length */
    swmm_SUBCATCH_CURB_LENGTH = 209,
    /*! \brief API provided rainfall */
    swmm_SUBCATCH_API_RAINFALL = 210,
    /*! \brief API provided snowfall */
    swmm_SUBCATCH_API_SNOWFALL = 211,
    /*! \brief Pollutant buildup */
    swmm_SUBCATCH_POLLUTANT_BUILDUP = 212,
    /*! \brief External pollutant buildup */
    swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP = 213,
    /*! \brief Runoff concentration */
    swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION = 214,
    /*! \brief Ponded concentration */
    swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION = 215,
    /*! \brief Total pollutant load */
    swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD = 216,
} swmm_SubcatchProperty;

/*!
 * \enum swmm_NodeProperty
 * \brief Enumeration of node properties used in SWMM5
 */
typedef enum
{
    /*! \brief Node type */
    swmm_NODE_TYPE = 300,
    /*! \brief Elevation */
    swmm_NODE_ELEV = 301,
    /*! \brief Maximum depth */
    swmm_NODE_MAXDEPTH = 302,
    /*! \brief Depth */
    swmm_NODE_DEPTH = 303,
    /*! \brief Hydraulic head */
    swmm_NODE_HEAD = 304,
    /*! \brief Volume */
    swmm_NODE_VOLUME = 305,
    /*! \brief Lateral inflow */
    swmm_NODE_LATFLOW = 306,
    /*! \brief Inflow */
    swmm_NODE_INFLOW = 307,
    /*! \brief Overflow */
    swmm_NODE_OVERFLOW = 308,
    /*! \brief Report flag */
    swmm_NODE_RPTFLAG = 309,
    /*! \brief Surcharge depth */
    swmm_NODE_SURCHARGE_DEPTH = 310,
    /*! \brief Ponded area */
    swmm_NODE_PONDED_AREA = 311,
    /*! \brief Initial depth */
    swmm_NODE_INITIAL_DEPTH = 312,
    /*! \brief Pollutant concentration */
    swmm_NODE_POLLUTANT_CONCENTRATION = 313,
    /*! \brief Pollutant lateral mass flux inflow */
    swmm_NODE_POLLUTANT_LATMASS_FLUX = 314,
} swmm_NodeProperty;

/*!
 * \enum swmm_LinkProperty
 * \brief Enumeration of link properties used in SWMM5
 */
typedef enum
{
    /*! \brief Link type */
    swmm_LINK_TYPE = 400,
    /*! \brief Upstream node */
    swmm_LINK_NODE1 = 401,
    /*! \brief Downstream node */
    swmm_LINK_NODE2 = 402,
    /*! \brief Length */
    swmm_LINK_LENGTH = 403,
    /*! \brief Slope */
    swmm_LINK_SLOPE = 404,
    /*! \brief Full depth */
    swmm_LINK_FULLDEPTH = 405,
    /*! \brief Full flow */
    swmm_LINK_FULLFLOW = 406,
    /*! \brief Setting */
    swmm_LINK_SETTING = 407,
    /*! \brief Time open */
    swmm_LINK_TIMEOPEN = 408,
    /*! \brief Time closed */
    swmm_LINK_TIMECLOSED = 409,
    /*! \brief Flow */
    swmm_LINK_FLOW = 410,
    /*! \brief Depth */
    swmm_LINK_DEPTH = 411,
    /*! \brief Velocity */
    swmm_LINK_VELOCITY = 412,
    /*! \brief Top width */
    swmm_LINK_TOPWIDTH = 413,
    /*! \brief Volume */
    swmm_LINK_VOLUME = 414,
    /*! \brief Capacity */
    swmm_LINK_CAPACITY = 415,
    /*! \brief Report flag */
    swmm_LINK_RPTFLAG = 416,
    /*! \brief Upstream invert offset */
    swmm_LINK_OFFSET1 = 417,
    /*! \brief Downstream invert offset */
    swmm_LINK_OFFSET2 = 418,
    /*! \brief Initial flow */
    swmm_LINK_INITIAL_FLOW = 419,
    /*! \brief Flow limit */
    swmm_LINK_FLOW_LIMIT = 420,
    /*! \brief Inlet loss */
    swmm_LINK_INLET_LOSS = 421,
    /*! \brief Outlet loss */
    swmm_LINK_OUTLET_LOSS = 422,
    /*! \brief Average loss */
    swmm_LINK_AVERAGE_LOSS = 423,
    /*! \brief Seepage rate */
    swmm_LINK_SEEPAGE_RATE = 424,
    /*! \brief Flap gate */
    swmm_LINK_HAS_FLAPGATE = 425,
    /*! \brief Pollutant concentration */
    swmm_LINK_POLLUTANT_CONCENTRATION = 426,
    /*! \brief Pollutant load */
    swmm_LINK_POLLUTANT_LOAD = 427,
    /*! \brief Pollutant lateral mass flux */
    swmm_LINK_POLLUTANT_LATMASS_FLUX = 428,
} swmm_LinkProperty;

/*!
 * \enum swmm_SystemProperty
 * \brief Enumeration of system properties used in SWMM5
 */
typedef enum
{
    /*! \brief Start date */
    swmm_STARTDATE = 0,
    /*! \brief Current date */
    swmm_CURRENTDATE = 1,
    /*! \brief Elapsed time */
    swmm_ELAPSEDTIME = 2,
    /*! \brief Routing step */
    swmm_ROUTESTEP = 3,
    /*! \brief Maximum routing step */
    swmm_MAXROUTESTEP = 4,
    /*! \brief Reporting step */
    swmm_REPORTSTEP = 5,
    /*! \brief Total steps */
    swmm_TOTALSTEPS = 6,
    /*! \brief No report flag */
    swmm_NOREPORT = 7,
    /*! \brief Flow units */
    swmm_FLOWUNITS = 8,
    /*! \brief End date */
    swmm_ENDDATE = 9,
    /*! \brief Report start */
    swmm_REPORTSTART = 10,
    /*! \brief Unit system */
    swmm_UNITSYSTEM = 11,
    /*! \brief Surcharge method */
    swmm_SURCHARGEMETHOD = 12,
    /*! \brief Allow ponding */
    swmm_ALLOWPONDING = 13,
    /*! \brief Inertia damping */
    swmm_INERTIADAMPING = 14,
    /*! \brief Normal flow limited */
    swmm_NORMALFLOWLTD = 15,
    /*! \brief Skip steady state */
    swmm_SKIPSTEADYSTATE = 16,
    /*! \brief Ignore rainfall */
    swmm_IGNORERAINFALL = 17,
    /*! \brief Ignore RDII */
    swmm_IGNORERDII = 18,
    /*! \brief Ignore snowmelt */
    swmm_IGNORESNOWMELT = 19,
    /*! \brief Ignore groundwater */
    swmm_IGNOREGROUNDWATER = 20,
    /*! \brief Ignore routing */
    swmm_IGNOREROUTING = 21,
    /*! \brief Ignore quality */
    swmm_IGNOREQUALITY = 22,
    /*! \brief Error code */
    swmm_ERROR_CODE = 23,
    /*! \brief Rule step */
    swmm_RULESTEP = 24,
    /*! \brief Sweep start */
    swmm_SWEEPSTART = 25,
    /*! \brief Sweep end */
    swmm_SWEEPEND = 26,
    /*! \brief Maximum trials */
    swmm_MAXTRIALS = 27,
    /*! \brief Number of threads */
    swmm_NUMTHREADS = 28,
    /*! \brief Minimum routing step */
    swmm_MINROUTESTEP = 29,
    /*! \brief Lengthening step */
    swmm_LENGTHENINGSTEP = 30,
    /*! \brief Start dry days */
    swmm_STARTDRYDAYS = 31,
    /*! \brief Courant factor */
    swmm_COURANTFACTOR = 32,
    /*! \brief Minimum surface area */
    swmm_MINSURFAREA = 33,
    /*! \brief Minimum slope */
    swmm_MINSLOPE = 34,
    /*! \brief Runoff error */
    swmm_RUNOFFERROR = 35,
    /*! \brief Flow error */
    swmm_FLOWERROR = 36,
    /*! \brief Quality error */
    swmm_QUALERROR = 37,
    /*! \brief Head tolerance */
    swmm_HEADTOL = 38,
    /*! \brief System flow tolerance */
    swmm_SYSFLOWTOL = 39,
    /*! \brief Lateral flow tolerance */
    swmm_LATFLOWTOL = 40,
} swmm_SystemProperty;

/*!
 * \enum swmm_FlowUnitsProperty
 * \brief Enumeration of flow units used in SWMM5
 */
typedef enum
{
    /*! \brief Cubic feet per second */
    swmm_CFS = 0,
    /*! \brief Gallons per minute */
    swmm_GPM = 1,
    /*! \brief Million gallons per day */
    swmm_MGD = 2,
    /*! \brief Cubic meters per second */
    swmm_CMS = 3,
    /*! \brief Liters per second */
    swmm_LPS = 4,
    /*! \brief Million liters per day */
    swmm_MLD = 5
} swmm_FlowUnitsProperty;

/*!
 * \enum swmm_API_Errors
 * \brief Enumeration of API errors used in SWMM5
 */
typedef enum
{
    /*! \brief API error for file not opened */
    ERR_API_NOT_OPEN = -999901,
    /*! \brief API error for API not started */
    ERR_API_NOT_STARTED = -999902,
    /*! \brief API error for API not ended */
    ERR_API_NOT_ENDED = -999903,
    /*! \brief API error for errorneous object type */
    ERR_API_OBJECT_TYPE = -999904,
    /*! \brief API error for errorneous object index */
    ERR_API_OBJECT_INDEX = -999905,
    /*! \brief API error for errorneous object name */
    ERR_API_OBJECT_NAME = -999906,
    /*! \brief API error for errorneous property type */
    ERR_API_PROPERTY_TYPE = -999907,
    /*! \brief API error for errorneous property value */
    ERR_API_PROPERTY_VALUE = -999908,
    /*! \brief API error for errorneous time period */
    ERR_API_TIME_PERIOD = -999909,
    /*! \brief API error for errorneous hotstart file open */
    ERR_API_HOTSTART_FILE_OPEN = -999910,
    /*! \brief API error for errorneous hotstart file format */
    ERR_API_HOTSTART_FILE_FORMAT = -999911,
    /*! \brief API error for API already running */
    ERR_API_IS_RUNNING = -999912,
} swmm_API_Errors;

/*!
 * \typedef progress_callback
 * \brief Callback function for progress reporting
 * \param[in] progress Progress value between 0 and 1
 */
typedef void (*progress_callback)(double progress);

/*!
 * \brief Run a SWMM simulation with the given input file, report file, and output file.
 * \param[in] inputFile Path to the input file
 * \param[in] reportFile Path to the report file
 * \param[in] outputFile Path to the output file
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_run(const char *inputFile, const char *reportFile, const char *outputFile);

/*!
 * \brief Run a SWMM simulation with the given input file, report file, and output file with a progress callback.
 * \param[in] inputFile Path to the input file
 * \param[in] reportFile Path to the report file
 * \param[in] outputFile Path to the output file
 * \param[in] callback Progress callback function
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_run_with_callback(
    const char *inputFile, const char *reportFile, const char *outputFile, progress_callback callback);

/*!
 * \brief Open a SWMM simulation with the given input file, report file, and output file.
 * \param[in] inputFile Path to the input file
 * \param[in] reportFile Path to the report file
 * \param[in] outputFile Path to the output file
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_open(const char *inputFile, const char *reportFile, const char *outputFile);

/*!
 * \brief Start a SWMM simulation with the given save flag.
 * \param[in] saveFlag Flag to save simulation
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_start(int saveFlag);

/*!
 * \brief Perform a SWMM simulation step and return the elapsed time.
 * \param[out] elapsedTime Elapsed time in decimal days
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_step(double *elapsedTime);

/*!
 * \brief Perform a SWMM simulation step with a stride step and return the elapsed time.
 * \param[in] strideStep Stride step
 * \param[out] elapsedTime Elapsed time
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_stride(int strideStep, double *elapsedTime);

/*!
 * \brief Set hotstart file for SWMM simulation.
 * \details Sets the hotstart file to use for simulation. Errors does not terminate simulation unless
 * there is a prior terminating error.
 * \param[in] hotStartFile Path to the hotstart file
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_useHotStart(const char *hotStartFile);

/*!
 * \brief Save hotstart file for SWMM simulation at current time.
 * \param[in] hotStartFile Path to the hotstart file
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_saveHotStart(const char *hotStartFile);

/*!
 * \brief End a SWMM simulation.
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_end(void);

/*!
 * \brief Writes simulation results to the report file.
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_report(void);

/*!
 * \brief Close a SWMM simulation.
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_close(void);

/*!
 * \brief Get the mass balance errors for a SWMM simulation.
 * \param[out] runoffErr Runoff error (percent)
 * \param[out] flowErr Flow error (percent)
 * \param[out] qualErr Quality error (percent)
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_getMassBalErr(float *runoffErr, float *flowErr, float *qualErr);

/*!
 * \brief Get the version of the SWMM engine.
 * \return Version number
 */
int EXPORT_SWMM_SOLVER_API swmm_getVersion(void);

/*!
 * \brief Retrieves the code number and text of the error condition that
 * caused SWMM to abort its analysis.
 * \param[out] errMsg Error message text
 * \param[in] msgLen Maximum size of errMsg
 * \return Error message code number
 */
int EXPORT_SWMM_SOLVER_API swmm_getError(char *errMsg, int msgLen);

/*!
 * \brief Retrieves the text of the error message that corresponds to the error code number.
 * \param[in] errorCode Error code number
 * \param[out] outErrMsg Error message text
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_getErrorFromCode(int error_code, char *outErrMsg[1024]);

/*!
 * \brief Gets the number of warnings issued during a simulation.
 * \return Number of warning messages issued
 */
int EXPORT_SWMM_SOLVER_API swmm_getWarnings(void);

/*!
 * \brief Retrieves the number of objects of a specific type.
 * \param[in] objType Type of SWMM object
 * \return Number of objects or error code
 */
int EXPORT_SWMM_SOLVER_API swmm_getCount(int objType);

/*!
 * \brief Retrieves the ID name of an object.
 * \param[in] objType Type of SWMM object
 * \param[in] index Object index
 * \param[out] name Object name
 * \param[in] size Size of the name array
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_getName(int objType, int index, char *name, int size);

/*!
 * \brief Retrieves the index of a named object.
 * \param[in] objType Type of SWMM object
 * \param[in] name Object name
 * \return Object index or error code
 */
int EXPORT_SWMM_SOLVER_API swmm_getIndex(int objType, const char *name);

/*!
 * \brief Get the value of a property for an object of a given property in the SWMM model.
 * \param[in] property Property type
 * \param[in] index Object index
 * \return Property value
 * \deprecated Use swmm_getValueExpanded instead. Function will be changed to swmm_getValueExpanded in future versions.
 */
double EXPORT_SWMM_SOLVER_API swmm_getValue(int property, int index);

/*!
 * \brief Get the value of a property for an object given property, index, and subindex in the SWMM model.
 * \param[in] objType Object type
 * \param[in] property Property type
 * \param[in] index Object index
 * \param[in] subIndex Optional Subindex for the property
 * \return Property value
 */
double EXPORT_SWMM_SOLVER_API swmm_getValueExpanded(int objType, int property, int index, int subIndex);

/*!
 * \brief Set the value of a property for an object of a given property and index in the SWMM model.
 * \param[in] property Property type
 * \param[in] index Object index
 * \param[in] value Property value
 * \return Error code
 * \deprecated Use swmm_setValueExpanded instead. Function will be changed to swmm_setValueExpanded in future versions.
 */
int EXPORT_SWMM_SOLVER_API swmm_setValue(int property, int index, double value);

/*!
 * \brief Set the value of a property for an object given property, index, and subindex in the SWMM model.
 * \param[in] objType Object type
 * \param[in] property Property type
 * \param[in] index Object index
 * \param[in] subIndex Optional Subindex for the property
 * \param[in] value Property value
 * \return Error code
 */
int EXPORT_SWMM_SOLVER_API swmm_setValueExpanded(int objType, int property, int index, int subIndex, double value);

/*!
 * \brief Get saved value of
 * \param[in] property Property type
 * \param[in] index Object index
 * \param[in] period Time period index
 * \return Property value
 */
double EXPORT_SWMM_SOLVER_API swmm_getSavedValue(int property, int index, int period);

/*!
 * \brief Write a line of text to the SWMM report file.
 * \param[in] line Line of text
 */
void EXPORT_SWMM_SOLVER_API swmm_writeLine(const char *line);

/*!
 * \brief Decode double date value into year, month, day, hour, minute, second, and day of week.
 * \param[in] date Date value
 * \param[out] year Year
 * \param[out] month Month
 * \param[out] day Day
 * \param[out] hour Hour
 * \param[out] minute Minute
 * \param[out] second Second
 * \param[out] dayOfWeek Day of week (0=Sunday, 1=Monday, ..., 6=Saturday)
 */
void EXPORT_SWMM_SOLVER_API swmm_decodeDate(double date, int *year, int *month, int *day,
                               int *hour, int *minute, int *second, int *dayOfWeek);

/*!
 * \brief Encode date values into a double date value.
 * \param[in] year Year
 * \param[in] month Month
 * \param[in] day Day
 * \param[in] hour Hour
 * \param[in] minute Minute
 * \param[in] second Second
 * \return Date value
 */
double EXPORT_SWMM_SOLVER_API swmm_encodeDate(int year, int month, int day,
                                 int hour, int minute, int second);

#ifdef __cplusplus
} // matches the linkage specification from above */
#endif

#endif // SWMM5_H
