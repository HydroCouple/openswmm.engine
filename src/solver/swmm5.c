/*!
 * \file swmm5.c
 * \brief Main module of the computational engine for Version 5 of the 
 * U.S. Environmental Protection Agency's Storm Water Management Model (SWMM).
 * \author L. Rossman
 * \date Created: 2021-11-01
 * \date Last edited: 2024-12-23
 * \version 5.3
 * \details This is the main module of the computational engine for Version 5 of
 * the U.S. Environmental Protection Agency's Storm Water Management Model
 * (SWMM). It contains functions that control the flow of computations.
 * 
 * This engine should be compiled into a shared object library whose API
 * functions are listed in swmm5.h.
 * 
 * Update History
 * ==============
 * Build 5.1.008:
 * - Support added for the MinGW compiler.
 * - Reporting of project options moved to swmm_start.
 * - Hot start file now read before routing system opened.
 * - Final routing step adjusted so that total duration not exceeded.
 * Build 5.1.011:
 * - Made sure that MS exception handling only used with MS C compiler.
 * - Added name of module handling an exception to error report.
 * - Elapsed simulation time now saved to new global variable ElaspedTime.
 * - Added swmm_getError() function that retrieves error code and message.
 * - Changed WarningCode to Warnings (# warnings issued).
 * - Added swmm_getWarnings() function to retrieve value of Warnings.
 * - Fixed error code returned on swmm_xxx functions.
 * Build 5.1.012:
 * - #include <direct.h> only used when compiled for Windows.
 * Build 5.1.013:
 * - Support added for saving average results within a reporting period.
 * - SWMM engine now always compiled to a shared object library.
 * Build 5.1.015:
 * - Fixes bug in summary statistics when Report Start date > Start Date.
 * Build 5.2.0:
 * - Added additional API functions.
 * - Set max. number of open files to 8192.
 * - Changed getElapsedTime function to use report start as base date/time.
 * - Prevented possible infinite loop if swmm_step() called when ErrorCode > 0.
 * - Prevented early exit from swmm_end() when ErrorCode > 0.
 * - Support added for relative file names.
 * Build 5.3.0:
 * - Added support for saving hot start files at specific times.
 * - Expanded SWMM api to save and use prescribed hotstart files.
 * - Expansions to the SWMM API to include attributes of more objects and water quality.
 */

/*!
* \def _CRT_SECURE_NO_DEPRECATE 
* \brief Define to prevent deprecation warnings from MS Visual C++ compilers
*/
#define _CRT_SECURE_NO_DEPRECATE

// --- define WINDOWS
#undef WINDOWS
#ifdef _WIN32

/*!
 * \def WINDOWS
 * \brief Define for Windows platform
 */
#define WINDOWS
#endif
#ifdef __WIN32__

/*!
 * \def WINDOWS
 * \brief Define for Windows platform
 */
#define WINDOWS
#endif

#undef EXH // indicates if exception handling included
#ifdef WINDOWS
#ifdef _MSC_VER
/*!
 * \def EXH
 * \brief Define for MS Windows exception handling
 */
#define EXH
#endif
#endif

// --- include Windows & exception handling headers
#ifdef WINDOWS
#include <windows.h>
#include <direct.h>
#include <errno.h>
#else
#include <unistd.h>
#endif
#ifdef EXH
#include <excpt.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <float.h>

// Protect against lack of compiler support for OpenMP
#if defined(_OPENMP)
#include <omp.h>     
#else
/*!
* \brief Dummy function for getting threads available for OpenMP to prevent compiler errors.
* \return 1
*/
int omp_get_max_threads(void) { return 1; }
#endif

//-----------------------------------------------------------------------------
//  SWMM's header files
//
//  Note: the directives listed below are also contained in headers.h which
//        is included at the start of most of SWMM's other code modules.
//-----------------------------------------------------------------------------
#include "macros.h"  // macros used throughout SWMM
#include "objects.h" // definitions of SWMM's data objects

/*!
* \def EXTERN
* \brief Define for 'extern' in headers.h
*/
#define EXTERN       // defined as 'extern' in headers.h

#include "globals.h" // declaration of all global variables
#include "funcs.h"   // declaration of all global functions
#include "error.h"   // error message codes
#include "text.h"    // listing of all text strings

#include "swmm5.h" // declaration of SWMM's API functions

/*!
* \def MAX_EXCEPTIONS
* \brief Maximum number of exceptions handled
*/
#define MAX_EXCEPTIONS 100 

/*!
* \addtogroup UnitConversionFactors Unit Conversion Factors
* \brief Conversion factors for units used in SWMM.
* \ingroup SWMM_Constants
* \{
*/

/*!
* \var Ucf
* \brief Unit conversion factors for different units used in SWMM.
*/
const double Ucf[10][2] =
    {
        //  US      SI
        {43200.0, 1097280.0},    // RAINFALL (in/hr, mm/hr --> ft/sec)
        {12.0, 304.8},           // RAINDEPTH (in, mm --> ft)
        {1036800.0, 26334720.0}, // EVAPRATE (in/day, mm/day --> ft/sec)
        {1.0, 0.3048},           // LENGTH (ft, m --> ft)
        {2.2956e-5, 0.92903e-5}, // LANDAREA (ac, ha --> ft2)
        {1.0, 0.02832},          // VOLUME (ft3, m3 --> ft3)
        {1.0, 1.608},            // WINDSPEED (mph, km/hr --> mph)
        {1.0, 1.8},              // TEMPERATURE (deg F, deg C --> deg F)
        {2.203e-6, 1.0e-6},      // MASS (lb, kg --> mg)
        {43560.0, 3048.0}        // GWFLOW (cfs/ac, cms/ha --> ft/sec)
};

/*!
* \var Qcf 
* \brief Flow conversion factors for different flow units used in SWMM.
*/
const double Qcf[6] =          // Flow Conversion Factors:
    {1.0, 448.831, 0.64632,    // cfs, gpm, mgd --> cfs
     0.02832, 28.317, 2.4466}; // cms, lps, mld --> cfs
/*!
* \}
*/

/*!
* \addtogroup SWMM_API_Shared_Variable SWMM Shared Variables
* \brief SWMM API shared variables.
* \ingroup SWMM_Constants
* \{
*/

/*!
* \var IsOpenFlag 
* \brief TRUE if a project has been opened.
*/
static int IsOpenFlag;         // TRUE if a project has been opened

/*!
* \var IsStartedFlag
* \brief TRUE if a simulation has been started.
*/
static int IsStartedFlag;      // TRUE if a simulation has been started

/*!
* \var SaveResultsFlag
* \brief TRUE if output to be saved to binary file.
*/
static int SaveResultsFlag;    // TRUE if output to be saved to binary file

/*!
* \var ExceptionCount
* \brief Number of exceptions handled.
*/
static int ExceptionCount;     // number of exceptions handled

/*!
* \var DoRunoff
* \brief TRUE if runoff is computed.
*/
static int DoRunoff;           // TRUE if runoff is computed

/*!
* \var DoRouting
* \brief TRUE if flow routing is computed.
*/
static int DoRouting;          // TRUE if flow routing is computed

/*!
* \var RoutingDuration
* \brief Duration of a set of routing steps (msecs).
*/
static double RoutingDuration; // duration of a set of routing steps (msecs)
/*!
* \}
*/

/*!
* \addtogroup SWMM_API_Local_Functions SWMM Local Functions
* \brief Local functions used by the SWMM API functions.
* \ingroup SWMM_Constants
* \{
*/

/*!
* \brief Routes flow and WQ through drainage system over a single time step.
*/
static void execRouting(void);

/*!
* \brief Saves current results to binary output file.
*/
static void saveResults(void);

/*!
* \brief Retrieve rain gage value given its index and property type.
* \param[in] index Object index
* \param[in] property Property type
* \return Property value
*/
static double getGageValue(int index, int property);

/*!
* \brief Retrieve subcatchment value given its index, property type, and subindex.
* \param[in] index Object index
* \param[in] property Property type
* \param[in] subIndex Subindex
* \return Property value
*/
static double getSubcatchValue(int property, int index, int subIndex);

/*!
* \brief Retrieve node value given its index, property type, and subindex.
* \param[in] index Object index
* \param[in] property Property type
* \param[in] subIndex Subindex
* \return Property value
*/
static double getNodeValue(int property, int index, int subIndex);

/*!
* \brief Retrieve link value given its index, property type, and subindex.
* \param[in] index Object index
* \param[in] property Property type
* \param[in] subIndex Subindex
* \return Property value
*/
static double getLinkValue(int property, int index, int subIndex);

/*!
* \brief Retrieve saved date value given a period.
* \param[in] period Period
* \return Date value
*/
static double getSavedDate(int period);

/*!
* \brief Retrieve saved value of a subcatchment, node, or link given its index, property type, and period.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] period Period
* \return Property value
*/
static double getSavedSubcatchValue(int property, int index, int period);

/*!
* \brief Retrieve saved value of a node given its index, property type, and period.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] period Period
* \return Property value
*/
static double getSavedNodeValue(int property, int index, int period);

/*!
* \brief Retrieve saved value of a link given its index, property type, and period.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] period Period
* \return Property value
*/
static double getSavedLinkValue(int property, int index, int period);

/*!
* \brief Retrieve system value given its property type.
* \param[in] property Property type
* \return Property value
*/
static double getSystemValue(int property);

/*!
* \brief Retrieve the maximum routing step.
* \return Maximum routing step
*/
static double getMaxRouteStep();

/*!
* \brief Set gage value given its property type, index, subindex, and value.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] subIndex Subindex
* \param[in] value Property value
* \return Error code
*/
static int setGageValue(int property, int index, int subIndex, double value);

/*!
* \brief Set subcatchment value given its property type, index, subindex, and value.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] subIndex Subindex
* \param[in] value Property value
* \return Error code
*/
static int setSubcatchValue(int property, int index, int subIndex, double value);

/*!
* \brief Set node value given its property type, index, subindex, and value.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] subIndex Subindex
* \param[in] value Property value
* \return Error code
*/
static int setNodeValue(int property, int index, int subIndex, double value);

/*!
* \brief Set link value given its property type, index, subindex, and value.
* \param[in] property Property type
* \param[in] index Object index
* \param[in] subIndex Subindex
* \param[in] value Property value
* \return Error code
*/
static int setLinkValue(int property, int index, int subIndex, double value);

/*!
* \brief Set node lateral inflow value given its index and value.
* \param[in] index Object index
* \param[in] value Property value
* \return Error code
*/
static int setNodeLatFlow(int index, double value);

/*!
* \brief Set outfall stage value given its index and value.
* \param[in] index Object index
* \param[in] value Property value
* \return Error code
*/
static int setOutfallStage(int index, double value);

/*!
* \brief Set link setting value given its index and value.
* \param[in] index Object index
* \param[in] value Property value
* \return Error code
*/
static int setLinkSetting(int index, double value);

/*!
* \brief Set routing step value.
* \param[in] value Property value
* \return Error code
*/
static int setRoutingStep(double value);

/*!
* \brief Set system value given its property type and value.
* \param[in] property Property type
* \param[in] value Property value
* \return Error code
*/
static int setSystemValue(int property, double value);

/*!
* \brief Get absolute path of a file.
* \param[in] fname File name
* \param[out] absPath Absolute path
* \param[in] size Size of the absPath array
*/
static void getAbsolutePath(const char *fname, char *absPath, size_t size);
/*!
* \}
*/

#ifdef EXH
/*!
* \brief Exception filter function
* \param[in] xc Exception code
* \param[in] module Module name
* \param[in] elapsedTime Elapsed time
* \param[in] step Step
* \return Error code
*/
static int xfilter(int xc, char *module, double elapsedTime, long step);
#endif

/*!
* \brief Run a SWMM simulation with the given input file, report file, and output file.
* \param[in] inputFile Path to the input file
* \param[in] reportFile Path to the report file
* \param[in] outputFile Path to the output file
* \return Error code
*/
int DLLEXPORT swmm_run(const char *inputFile, const char *reportFile, const char *outputFile)
{
    long newHour, oldHour = 0;
    long theDay, theHour;
    double elapsedTime = 0.0;

    // --- initialize flags
    IsOpenFlag = FALSE;
    IsStartedFlag = FALSE;
    SaveResultsFlag = TRUE;

    // --- open the files & read input data
    ErrorCode = 0;
    writecon("\n o  Retrieving project data");
    swmm_open(inputFile, reportFile, outputFile);

    // --- run the simulation if input data OK
    if (!ErrorCode)
    {
        // --- initialize values
        swmm_start(TRUE);

        // --- execute each time step until elapsed time is re-set to 0
        if (!ErrorCode)
        {
            writecon("\n o  Simulating day: 0     hour:  0");
            do
            {
                swmm_step(&elapsedTime);
                newHour = (long)(elapsedTime * 24.0);
                if (newHour > oldHour)
                {
                    theDay = (long)elapsedTime;
                    theHour = (long)((elapsedTime - floor(elapsedTime)) * 24.0);
                    writecon("\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
                    snprintf(Msg, MAXMSG, "%-5ld hour: %-2ld", theDay, theHour);
                    writecon(Msg);
                    oldHour = newHour;
                }
            } while (elapsedTime > 0.0 && !ErrorCode);
            writecon("\b\b\b\b\b\b\b\b\b\b\b\b\b\b"
                     "\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
            writecon("Simulation complete           ");
        }

        // --- clean up
        swmm_end();
    }

    // --- report results
    if (!ErrorCode && Fout.mode == SCRATCH_FILE)
    {
        writecon("\n o  Writing output report");
        swmm_report();
    }

    // --- close the system
    swmm_close();
    return ErrorCode;
}

/*!
* \brief Run a SWMM simulation with the given input file, report file, output file, and progress callback function.
* \param[in] inputFile Path to the input file
* \param[in] reportFile Path to the report file
* \param[in] outputFile Path to the output file
* \param[in] callback Progress callback function
* \return Error code
*/
int DLLEXPORT swmm_run_with_callback(const char *inputFile, const char *reportFile, const char *outputFile, progress_callback callback)
{
    double progress = 0.0, elapsedTime = 0.0;

    // --- initialize flags
    IsOpenFlag = FALSE;
    IsStartedFlag = FALSE;
    SaveResultsFlag = TRUE;

    // --- open the files & read input data
    ErrorCode = 0;

    swmm_open(inputFile, reportFile, outputFile);

    // --- run the simulation if input data OK
    if (!ErrorCode)
    {
        // --- initialize values
        swmm_start(TRUE);

        // --- execute each time step until elapsed time is re-set to 0
        if (!ErrorCode)
        {
            do
            {
                swmm_step(&elapsedTime);

                // --- calculate progress
                if (callback != NULL)
                {
                    progress = NewRoutingTime / TotalDuration;
                    callback(progress);
                }

            } while (elapsedTime > 0.0 && !ErrorCode);
        }

        // --- clean up
        swmm_end();
    }

    // --- report results
    if (!ErrorCode && Fout.mode == SCRATCH_FILE)
    {
        swmm_report();
    }

    // --- close the system
    swmm_close();

    return ErrorCode;
}

/*!
* \brief Open a SWMM project.
* \param[in] inputFile Path to the input file
* \param[in] reportFile Path to the report file
* \param[in] outputFile Path to the output file
* \return Error code
*/
int DLLEXPORT swmm_open(const char *inputFile, const char *reportFile, const char *outputFile)
{
// --- to be safe, reset the state of the floating point unit
#ifdef WINDOWS
    _fpreset();
    _setmaxstdio(8192);
#endif

#ifdef EXH
    // --- begin exception handling here
    __try
#endif
    {
        // --- initialize error & warning codes
        datetime_setDateFormat(M_D_Y);
        ErrorCode = 0;
        ErrorMsg[0] = '\0';
        Warnings = 0;
        IsOpenFlag = FALSE;
        IsStartedFlag = FALSE;
        ExceptionCount = 0;

        // --- open a SWMM project
        strcpy(InpDir, "");
        project_open(inputFile, reportFile, outputFile);
        getAbsolutePath(inputFile, InpDir, sizeof(InpDir));
        if (ErrorCode)
            return ErrorCode;
        IsOpenFlag = TRUE;
        report_writeLogo();

        // --- retrieve project data from input file
        project_readInput();
        if (ErrorCode)
            return ErrorCode;

        // --- write project title to report file & validate data
        report_writeTitle();
        project_validate();
    }

#ifdef EXH
    // --- end of try loop; handle exception here
    __except (xfilter(GetExceptionCode(), "swmm_open", 0.0, 0))
    {
        ErrorCode = ERR_SYSTEM;
    }
#endif
    return ErrorCode;
}

/*!
* \brief Start a SWMM simulation.
* \param[in] saveResults TRUE if simulation results saved to binary file
* \return Error code
*/
int DLLEXPORT swmm_start(int saveResults)
{
    // --- check that a project is open & no run started
    if (ErrorCode)
        return ErrorCode;
    if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);
    if (IsStartedFlag)
        return (ErrorCode = ERR_API_NOT_ENDED);

    // --- write input summary & project options to report file if requested
    if (!RptFlags.disabled)
    {
        if (RptFlags.input)
            inputrpt_writeInput();
        report_writeOptions();
    }

    // --- save saveResults flag to global variable
    SaveResultsFlag = saveResults;
    ExceptionCount = 0;

#ifdef EXH
    // --- begin exception handling loop here
    __try
#endif
    {
        // --- initialize elapsed time in decimal days
        ElapsedTime = 0.0;
        RoutingDuration = TotalDuration;

        // --- initialize runoff, routing & reporting time (in milliseconds)
        NewRunoffTime = 0.0;
        NewRoutingTime = 0.0;
        ReportTime = 1000 * (double)ReportStep;
        TotalStepCount = 0;
        ReportStepCount = 0;
        NonConvergeCount = 0;
        IsStartedFlag = TRUE;

        // --- initialize global continuity errors
        RunoffError = 0.0;
        GwaterError = 0.0;
        FlowError = 0.0;
        QualError = 0.0;

        // --- open rainfall processor (creates/opens a rainfall
        //     interface file and generates any RDII flows)
        if (!IgnoreRainfall)
            rain_open();
        if (ErrorCode)
            return ErrorCode;

        // --- initialize state of each major system component
        project_init();

        // --- see if runoff & routing needs to be computed
        if (Nobjects[SUBCATCH] > 0)
            DoRunoff = TRUE;
        else
            DoRunoff = FALSE;
        if (Nobjects[NODE] > 0 && !IgnoreRouting)
            DoRouting = TRUE;
        else
            DoRouting = FALSE;

        // --- open binary output file
        output_open();

        // --- open runoff processor
        if (DoRunoff)
            runoff_open();

        // --- open & read hot start file if present
        if (!hotstart_open())
            return ErrorCode;

        // --- open routing processor
        if (DoRouting)
            routing_open();

        // --- open mass balance and statistics processors
        massbal_open();
        stats_open();

        // --- write heading for control actions listing
        if (!RptFlags.disabled && RptFlags.controls)
            report_writeControlActionsHeading();
    }

#ifdef EXH
    // --- end of try loop; handle exception here
    __except (xfilter(GetExceptionCode(), "swmm_start", 0.0, 0))
    {
        ErrorCode = ERR_SYSTEM;
    }
#endif
    return ErrorCode;
}

/*!
* \brief Perform a SWMM simulation step and return the elapsed time.
* \param[out] elapsedTime Elapsed time
* \return Error code 
*/
int DLLEXPORT swmm_step(double *elapsedTime)
//
//  Input:   elapsedTime = current elapsed time in decimal days
//  Output:  updated value of elapsedTime,
//           returns error code
//  Purpose: advances the simulation by one routing time step.
//
{
    // --- check that simulation can proceed
    *elapsedTime = 0.0;
    if (ErrorCode)
        return ErrorCode;
    if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);
    if (!IsStartedFlag)
        return (ErrorCode = ERR_API_NOT_STARTED);

#ifdef EXH
    // --- begin exception handling loop here
    __try
#endif
    {
        // --- if routing time has not exceeded total duration
        if (NewRoutingTime < RoutingDuration)
        {
            // --- route flow & WQ through drainage system
            //     (runoff will be calculated as needed)
            //     (NewRoutingTime is updated)
            execRouting();
        }

        // --- if saving results to the binary file
        if (SaveResultsFlag)
            saveResults();

        // --- save hostart files if applicable
        hotstart_save();

        // --- update elapsed time (days)
        if (NewRoutingTime < RoutingDuration)
            ElapsedTime = NewRoutingTime / MSECperDAY;

        // --- otherwise end the simulation
        else
            ElapsedTime = 0.0;
        *elapsedTime = ElapsedTime;
    }

#ifdef EXH
    // --- end of try loop; handle exception here
    __except (xfilter(GetExceptionCode(), "swmm_step", ElapsedTime, TotalStepCount))
    {
        ErrorCode = ERR_SYSTEM;
    }
#endif
    return ErrorCode;
}

/*!
* \brief Perform a SWMM simulation step with a stride step and return the elapsed time.
* \param[in] strideStep Stride step
* \param[out] elapsedTime Elapsed time
* \return Error code 
*/
int DLLEXPORT swmm_stride(int strideStep, double *elapsedTime)
{
    double realRouteStep = RouteStep;

    // --- check that simulation can proceed
    *elapsedTime = 0.0;
    if (ErrorCode)
        return ErrorCode;
    if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);
    if (!IsStartedFlag)
        return (ErrorCode = ERR_API_NOT_STARTED);

    // --- modify total duration to be strideStep seconds after current time
    RoutingDuration = NewRoutingTime + 1000.0 * strideStep;
    RoutingDuration = MIN(TotalDuration, RoutingDuration);

    // --- modify routing step to not exceed stride time step
    if (strideStep < RouteStep)
        RouteStep = strideStep;

    // --- step through simulation until next stride step is reached
    do
    {
        swmm_step(elapsedTime);
    } while (*elapsedTime > 0.0 && !ErrorCode);

    // --- restore original routing step and routing duration
    RouteStep = realRouteStep;
    RoutingDuration = TotalDuration;

    // --- restore actual elapsed time (days)
    if (NewRoutingTime < TotalDuration)
    {
        ElapsedTime = NewRoutingTime / MSECperDAY;
    }
    else
        ElapsedTime = 0.0;
    *elapsedTime = ElapsedTime;
    return ErrorCode;
}

/*!
* \brief Set hotstart file for SWMM simulation.
* \details Sets the hotstart file to use for simulation. Errors does not terminate simulation unless
* there is a prior terminating error.
* \param[in] hotStartFile Path to the hotstart file
* \return Error code 
*/
int DLLEXPORT swmm_useHotStart(const char *hotStartFile)
{
    int errorCode = 0;
    int fileVersion = 0;
    char fname[MAXFNAME + 1];

    if (ErrorCode)
        return ErrorCode;
    else if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);
    else if (IsStartedFlag)
        return (ErrorCode = ERR_API_NOT_ENDED);

    sstrncpy(fname, hotStartFile, MAXFNAME);

    // Try to open the hotstart file first to see if it is valid
    errorCode = hotstart_is_valid(addAbsolutePath(fname), &fileVersion);
    if (errorCode)
    {
        return errorCode;
    }
    else
    {
        FhotstartInput.mode = USE_FILE;
        sstrncpy(FhotstartInput.name, addAbsolutePath(fname), MAXFNAME);
    }

    return errorCode;
}

/*!
* \brief Save hotstart file for SWMM simulation at current time.
* \param[in] hotStartFile Path to the hotstart file
* \return Error code 
*/
int DLLEXPORT swmm_saveHotStart(const char *hotStartFile)
{
    int errorCode = 0;

    if (ErrorCode)
        return ErrorCode;
    else if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);
    else if (!IsStartedFlag)
        return (ErrorCode = ERR_API_NOT_STARTED);

    errorCode = hotstart_save_to_file(hotStartFile);

    return errorCode;
}

/*!
* \brief Routes flow and WQ through drainage system over a single time step.
*/
void execRouting()
{
    double nextRoutingTime; // updated elapsed routing time (msec)
    double routingStep;     // routing time step (sec)

#ifdef EXH
    // --- begin exception handling loop here
    __try
#endif
    {
        // --- determine when next routing time occurs
        TotalStepCount++;
        if (!DoRouting)
            routingStep = MIN(WetStep, ReportStep);
        else
            routingStep = routing_getRoutingStep(RouteModel, RouteStep);
        if (routingStep <= 0.0)
        {
            ErrorCode = ERR_TIMESTEP;
            return;
        }
        nextRoutingTime = NewRoutingTime + 1000.0 * routingStep;

        // --- adjust routing step so that total duration not exceeded
        if (nextRoutingTime > RoutingDuration)
        {
            routingStep = (RoutingDuration - NewRoutingTime) / 1000.0;
            routingStep = MAX(routingStep, 1. / 1000.0);
            nextRoutingTime = RoutingDuration;
        }

        // --- compute runoff until next routing time reached or exceeded
        if (DoRunoff)
            while (NewRunoffTime < nextRoutingTime)
            {
                runoff_execute();
                if (ErrorCode)
                    return;
            }

        // --- if no runoff analysis, update climate state (for evaporation)
        else
            climate_setState(getDateTime(NewRoutingTime));

        // --- route flows & pollutants through drainage system
        //     (while updating NewRoutingTime)
        if (DoRouting)
            routing_execute(RouteModel, routingStep);
        else
            NewRoutingTime = nextRoutingTime;
    }

#ifdef EXH
    // --- end of try loop; handle exception here
    __except (xfilter(GetExceptionCode(), "execRouting",
                      ElapsedTime, TotalStepCount))
    {
        ErrorCode = ERR_SYSTEM;
        return;
    }
#endif
}

/*!
* \brief Saves current results to binary output file.
*/
void saveResults()
{
    if (NewRoutingTime >= ReportTime)
    {
        // --- if user requested that average results be saved:
        if (RptFlags.averages)
        {
            // --- include latest results in current averages
            //     if current time equals the reporting time
            if (NewRoutingTime == ReportTime)
                output_updateAvgResults();

            // --- save current average results to binary file
            //     (which will re-set averages to 0)
            output_saveResults(ReportTime);

            // --- if current time exceeds reporting period then
            //     start computing averages for next period
            if (NewRoutingTime > ReportTime)
                output_updateAvgResults();
        }

        // --- otherwise save interpolated point results
        else
            output_saveResults(ReportTime);

        // --- advance to next reporting period
        ReportTime = ReportTime + 1000 * (double)ReportStep;
    }

    // --- not a reporting period so update average results if applicable
    else if (RptFlags.averages)
        output_updateAvgResults();
}

/*!
* \brief End a SWMM simulation.
* \return Error code
*/
int DLLEXPORT swmm_end(void)
{
    // --- check that project opened and run started
    if (!IsOpenFlag)
        return (ErrorCode = ERR_API_NOT_OPEN);

    if (IsStartedFlag)
    {
        // --- write ending records to binary output file
        if (Fout.file)
            output_end();

        // --- report mass balance results and system statistics
        if (!ErrorCode && RptFlags.disabled == 0)
        {
            massbal_report();
            stats_report();
        }

        // --- close all computing systems
        stats_close();
        massbal_close();
        if (!IgnoreRainfall)
            rain_close();
        if (DoRunoff)
            runoff_close();
        if (DoRouting)
            routing_close(RouteModel);
        hotstart_close();
        IsStartedFlag = FALSE;
    }
    return ErrorCode;
}

/*!
* \brief Writes simulation results to the report file.
* \return Error code 
*/
int DLLEXPORT swmm_report()
{
    if (!ErrorCode)
        report_writeReport();
    return ErrorCode;
}

/*!
* \brief Write a line of text to the SWMM report file.
* \param[in] line Line of text
*/
void DLLEXPORT swmm_writeLine(const char *line)
{
    if (IsOpenFlag)
        report_writeLine(line);
}

/*!
* \brief Close a SWMM simulation.
* \return Error code 
*/
int DLLEXPORT swmm_close()
{
    if (Fout.file)
        output_close();
    if (IsOpenFlag)
        project_close();
    report_writeSysTime();
    if (Finp.file != NULL)
        fclose(Finp.file);
    if (Frpt.file != NULL)
        fclose(Frpt.file);
    if (Fout.file != NULL)
    {
        fclose(Fout.file);
        if (Fout.mode == SCRATCH_FILE)
            remove(Fout.name);
    }
    IsOpenFlag = FALSE;
    IsStartedFlag = FALSE;
    return 0;
}

/*!
* \brief Get the mass balance errors for a SWMM simulation.
* \param[out] runoffErr Runoff error (percent)
* \param[out] flowErr Flow error (percent)
* \param[out] qualErr Quality error (percent)
* \return Error code 
*/
int DLLEXPORT swmm_getMassBalErr(float *runoffErr, float *flowErr,
                                 float *qualErr)
{
    *runoffErr = 0.0;
    *flowErr = 0.0;
    *qualErr = 0.0;

    if (IsOpenFlag && !IsStartedFlag)
    {
        *runoffErr = (float)RunoffError;
        *flowErr = (float)FlowError;
        *qualErr = (float)QualError;
    }
    return 0;
}

/*!
* \brief Gets the version of the SWMM engine.
* \details Retrieves the version number of the current SWMM engine which uses a 
format of xyzzz where x = major version number, y = minor version number, and 
zzz = build number.
* \note Each New Release should be updated in consts.h
* \return SWMM engine version number
*/
int DLLEXPORT swmm_getVersion()
{
    return VERSION;
}

/*!
* \brief Gets the number of warnings issued during a simulation.
* \return Number of warning messages issued
*/
int DLLEXPORT swmm_getWarnings()
{
    return Warnings;
}

/*!
* \brief Retrieves the code number and text of the error condition that 
* caused SWMM to abort its analysis.
* \param[out] errMsg Error message text
* \param[in] msgLen Maximum size of errMsg
* \return Error message code number
*/
int DLLEXPORT swmm_getError(char *errMsg, int msgLen)
{
    // --- copy text of last error message into errMsg
    if (ErrorCode > 0 && strlen(ErrorMsg) == 0)
        error_getMsg(ErrorCode, ErrorMsg);

    sstrncpy(errMsg, ErrorMsg, msgLen);

    // --- remove leading line feed from errMsg
    if (msgLen > 0 && errMsg[0] == '\n')
        errMsg[0] = ' ';
    return ErrorCode;
}

/*!
* \brief Retrieves the text of the error message that corresponds to the error code number.
* \param[in] errorCode Error code number
* \param[out] outErrMsg Error message text
* \return Error code
*/
int DLLEXPORT swmm_getErrorFromCode(int errorCode, char *outErrMsg[1024])
{
    char err_msg[MAXMSG];
    error_getMsg(errorCode, err_msg);
    sstrncpy(*outErrMsg, err_msg, MAXMSG);

    return 0;
}

/*!
* \brief Retrieves the number of objects of a specific type.
* \param[in] objType Type of SWMM object
* \return Number of objects or error code
*/
int DLLEXPORT swmm_getCount(int objType)
{
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;
    if (objType < swmm_GAGE || objType >= swmm_SYSTEM)
        return ERR_API_OBJECT_TYPE;
    return Nobjects[objType];
}

/*!
* \brief Retrieves the ID name of an object.
* \param[in] objType Type of SWMM object
* \param[in] index Object index
* \param[out] name Object name
* \param[in] size Size of the name array
* \return Error code
*/
int DLLEXPORT swmm_getName(int objType, int index, char *name, int size)
{
    char *idName = NULL;

    name[0] = '\0';
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;

    switch (objType)
    {
    case GAGE:
        idName = Gage[index].ID;
        break;
    case SUBCATCH:
        idName = Subcatch[index].ID;
        break;
    case NODE:
        idName = Node[index].ID;
        break;
    case LINK:
        idName = Link[index].ID;
        break;
    case POLLUT:
        idName = Pollut[index].ID;
        break;
    case LANDUSE:
        idName = Landuse[index].ID;
        break;
    case TIMEPATTERN:
        idName = Pattern[index].ID;
        break;
    case CURVE:
        idName = Curve[index].ID;
        break;
    case TSERIES:
        idName = Tseries[index].ID;
        break;
    case TRANSECT:
        idName = Transect[index].ID;
        break;
    case AQUIFER:
        idName = Aquifer[index].ID;
        break;
    case UNITHYD:
        idName = UnitHyd[index].ID;
        break;
    case SNOWMELT:
        idName = Snowmelt[index].ID;
        break;
    default:
        return ERR_API_OBJECT_TYPE;
    }

    if (idName)
        sstrncpy(name, idName, size);

    return 0;
}

/*!
* \brief Retrieves the index of a named object.
* \param[in] objType Type of SWMM object
* \param[in] name Object name
* \return Object index or error code
*/
int DLLEXPORT swmm_getIndex(int objType, const char *name)
{
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;
    if (objType < swmm_GAGE || objType >= swmm_SYSTEM)
        return ERR_API_OBJECT_TYPE;
    return project_findObject(objType, name);
}

/*!
* \brief Get the value of a property for an object of a given property in the SWMM model.
* \param[in] property Property type
* \param[in] index Object index
* \return Property value
* \deprecated Use swmm_getValueExpanded instead. Function will be changed to 
* swmm_getValueExpanded in future versions.
*/
double DLLEXPORT swmm_getValue(int property, int index)
{
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;
    if (property < 100)
        return getSystemValue(property);
    if (property < 200)
        return getGageValue(property, index);
    if (property < 300)
        return getSubcatchValue(property, index, -1);
    if (property < 400)
        return getNodeValue(property, index, -1);
    if (property < 500)
        return getLinkValue(property, index, -1);

    return ERR_API_PROPERTY_TYPE;
}

/*!
* \brief Get the value of a property for an object of a given property in the SWMM model.
* \param[in] objType Type of SWMM object
* \param[in] property Property type
* \param[in] index Object index in the array of like objects
* \param[in] subIndex Subindex
* \return Property value
*/
double DLLEXPORT swmm_getValueExpanded(int objType, int property, int index, int subIndex)
{
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;

    switch (objType)
    {
    case swmm_SYSTEM:
        return getSystemValue(property);
    case swmm_GAGE:
        return getGageValue(property, index);
    case swmm_SUBCATCH:
        return getSubcatchValue(property, index, subIndex);
    case swmm_NODE:
        return getNodeValue(property, index, subIndex);
    case swmm_LINK:
        return getLinkValue(property, index, subIndex);
    default:
        return ERR_API_OBJECT_TYPE;
    }
}

/*!
* \brief Set the value of a property for an object of a given property in the SWMM model.
* \param[in] property Property type
* \param[in] index Object index in the array of like objects
* \param[in] value Property value   
* \return Error code
* \deprecated Use swmm_setValueExpanded instead. Function will be changed to
* swmm_setValueExpanded in future versions.
*/
int DLLEXPORT swmm_setValue(int property, int index, double value)
{

    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;

    switch (property)
    {
    case swmm_GAGE_RAINFALL:
        if (index < 0 || index >= Nobjects[GAGE])
            return 0;
        if (value >= 0.0)
            Gage[index].apiRainfall = value;
        return 0;
    case swmm_SUBCATCH_RPTFLAG:
        if (!IsStartedFlag && index >= 0 && index < Nobjects[SUBCATCH])
            Subcatch[index].rptFlag = (value > 0.0);
        return 0;
    case swmm_NODE_LATFLOW:
        setNodeLatFlow(index, value);
        return 0;
    case swmm_NODE_HEAD:
        setOutfallStage(index, value);
        return 0;
    case swmm_NODE_RPTFLAG:
        if (!IsStartedFlag && index >= 0 && index < Nobjects[NODE])
            Node[index].rptFlag = (value > 0.0);
        return 0;
    case swmm_LINK_SETTING:
        setLinkSetting(index, value);
        return 0;
    case swmm_LINK_RPTFLAG:
        if (!IsStartedFlag && index >= 0 && index < Nobjects[LINK])
            Link[index].rptFlag = (value > 0.0);
        return 0;
    case swmm_ROUTESTEP:
        setRoutingStep(value);
        return 0;
    case swmm_REPORTSTEP:
        if (!IsStartedFlag && value > 0)
            ReportStep = (int)value;
        return 0;
    case swmm_NOREPORT:
        if (!IsStartedFlag)
            RptFlags.disabled = (value > 0.0);
        return 0;
	default:
		return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================

int DLLEXPORT swmm_setValueExpanded(int objType, int property, int index, int subIndex, double value)
//
//  Input:   objType = a type of SWMM object
//           property = an object's property code
//           index = the object's index in the array of like objects
//           subIndex = an index of the object's property
//           value = the property's new value
//  Output:  returns error code
//  Purpose: sets the value of an object's property.
//  TODO:    Will be deprecated in SWMM version 6.0
{
    if (!IsOpenFlag)
        return ERR_API_NOT_OPEN;

    switch (index)
    {
    case swmm_SYSTEM:
        return setSystemValue(property, value);
    case swmm_GAGE:
        return setGageValue(property, index, subIndex, value);
    case swmm_SUBCATCH:
        return setSubcatchValue(property, index, subIndex, value);
    case swmm_NODE:
        return setNodeValue(property, index, subIndex, value);
    case swmm_LINK:
        return setLinkValue(property, index, subIndex, value);
    default:
        return ERR_API_OBJECT_TYPE;
    }
}

//=============================================================================

int setGageValue(int property, int index, int subIndex, double value)
//
//  Input:   property = an object's property code
//           index = the object's index in the array of like objects
//           subIndex = an index of the object's property
//           value = the property's new value
//  Output:  returns error code
//  Purpose: sets the value of a rain gage object's property.
{
    if (index < 0 || index >= Nobjects[GAGE])
        return ERR_API_OBJECT_INDEX;

    switch (property)
    {
    case swmm_GAGE_RAINFALL:
        if (value >= 0.0)
        {
            Gage[index].apiRainfall = value;
            return 0;
        }
        else
            return ERR_API_PROPERTY_VALUE;
    default:
        return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================

int setSubcatchValue(int property, int index, int subIndex, double value)
//
//  Input:   property = an object's property code
//           index = the object's index in the array of like objects
//           subIndex = an index of the object's property
//           value = the property's new value
//  Output:  returns error code
//  Purpose: sets the value of a subcatchment object's property.
//  TODO: Convert Error codes to warnings
{

    if (IsOpenFlag == FALSE)
    {
        return ERR_API_NOT_OPEN;
    }
        // Check if object index is within bounds
    else if (index < 0 || index >= Nobjects[SUBCATCH])
        return ERR_API_OBJECT_INDEX;
    // Check if Simulation is Running
    else if (IsStartedFlag == TRUE)
    {
        // Set values that can be configured at runtime during simulation
        switch (property)
        {
		case swmm_SUBCATCH_API_RAINFALL:
            if (value >= 0.0)
            {
                Subcatch[index].apiRainfall = value / UCF(RAINFALL);
                return 0;
            }
			else
				return ERR_API_PROPERTY_VALUE;
		case swmm_SUBCATCH_API_SNOWFALL:
			if (value >= 0.0)
			{
				Subcatch[index].apiSnowfall = value / UCF(RAINFALL);
				return 0;
			}
			else
				return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP:
            Subcatch[index].apiExtBuildup[subIndex] = value;
            return 0;
        default:
            return ERR_API_IS_RUNNING;
        }
    }
    else
    {
        switch (property)
        {
        case swmm_SUBCATCH_AREA:
            if (value >= 0.0)
            {
                Subcatch[index].area = value / UCF(LANDAREA);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_WIDTH:
            if (value >= 0.0)
            {
                Subcatch[index].width = value / UCF(LENGTH);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
		case swmm_SUBCATCH_SLOPE:
			if (value >= 0.0)
			{
				Subcatch[index].slope = value;
				return 0;
			}
			else
				return ERR_API_PROPERTY_VALUE;
		case swmm_SUBCATCH_CURB_LENGTH:
			if (value >= 0.0)
			{
				Subcatch[index].curbLength = value / UCF(LENGTH);
				return 0;
			}
			else
				return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_API_RAINFALL:
            if (value >= 0.0)
            {
                Subcatch[index].apiRainfall = value / UCF(RAINFALL);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_API_SNOWFALL:
            if (value >= 0.0)
            {
                Subcatch[index].apiSnowfall = value / UCF(RAINFALL);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_RPTFLAG:
            if (value >= 0.0)
            {
                Subcatch[index].rptFlag = (value > 0.0);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP:
            Subcatch[index].apiExtBuildup[subIndex] = value;
            return 0;
        default:
            return ERR_API_PROPERTY_TYPE;
        }
    }
}

//=============================================================================

int setNodeValue(int property, int index, int subIndex, double value)
//
//  Input:   property = an object's property code
//           index = the object's index in the array of like objects
//           subIndex = an index of the object's property
//           value = the property's new value
//  Output:  returns error code
//  Purpose: sets the value of a node object's property.
//  TODO: Convert Error codes to warnings
{

    TNode *node = NULL;

    if (IsOpenFlag == FALSE)
    {
        return ERR_API_NOT_OPEN;
    }
    // Check if object index is within bounds
    else if (index < 0 || index >= Nobjects[NODE])
    {   
        return ERR_API_OBJECT_INDEX;
    }
    // Check if Simulation is Running
    else if (IsStartedFlag == TRUE)
    {

        // Set values that can be configured at runtime during simulation
        switch (property)
        {
        case swmm_NODE_LATFLOW:
            Node[index].apiExtInflow = value / UCF(FLOW);
            return 0;
        case swmm_NODE_HEAD:
            node = &Node[index];

            if (node->type != OUTFALL)
                return ERR_API_OBJECT_TYPE;

            Outfall[node->subIndex].fixedStage = value / UCF(LENGTH);
            Outfall[node->subIndex].type = FIXED_OUTFALL;
            return 0;
        case swmm_NODE_POLLUTANT_LATMASS_FLUX:
            Node[index].apiExtQualMassFlux[subIndex] = value;
            return 0;
        default:
            return ERR_API_IS_RUNNING;
        }
    }
    else
    {   
        // Set values that can only be configured before simulation starts
        switch (property)
        {
        case swmm_NODE_ELEV:
            Node[index].invertElev = value / UCF(LENGTH);
            return 0;
        case swmm_NODE_MAXDEPTH:
            if (value >= 0.0)
            {
                Node[index].fullDepth = value / UCF(LENGTH);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_NODE_SURCHARGE_DEPTH:
            if (value >= 0.0)
            {
                Node[index].surDepth = value / UCF(LENGTH);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_NODE_PONDED_AREA:
            if (value >= 0.0)
            {
                Node[index].pondedArea = value / UCF(LANDAREA);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_NODE_INITIAL_DEPTH:
            if (value >= 0.0)
            {
                Node[index].initDepth = value / UCF(LENGTH);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
        case swmm_NODE_LATFLOW:
            Node[index].apiExtInflow = value / UCF(FLOW);
            return 0;
        case swmm_NODE_HEAD:
            node = &Node[index];

            if (node->type != OUTFALL)
                return ERR_API_OBJECT_TYPE;

            Outfall[node->subIndex].fixedStage = value / UCF(LENGTH);
            Outfall[node->subIndex].type = FIXED_OUTFALL;
            return 0;
        case swmm_NODE_RPTFLAG:
            Node[index].rptFlag = (value > 0.0);
            return 0;
        case swmm_NODE_POLLUTANT_LATMASS_FLUX:
            Node[index].apiExtQualMassFlux[subIndex] = value;
            return 0;
        default:
            return ERR_API_PROPERTY_TYPE;
        }
    }
}

//=============================================================================

int setLinkValue(int property, int index, int subIndex, double value)
//
//  Input:   property = an object's property code
//           index = the object's index in the array of like objects
//           subIndex = an index of the object's property
//           value = the property's new value
//  Output:  returns error code
//  Purpose: sets the value of a link object's property.
//  TODO: Convert Error codes to warnings
{
    TLink *link = NULL;
    const char* control_rule_label = "SWMM API";
    
    if (IsOpenFlag == FALSE)
    {
        return ERR_API_NOT_OPEN;
    }
    // Check if object index is within bounds
    else if (index < 0 || index >= Nobjects[LINK])
    {
        return ERR_API_OBJECT_INDEX;
    }
    // Check if Simulation is Running
    else if (IsStartedFlag == TRUE)
    {

        link = &Link[index];
        
        // Set values that can be configured at runtime during simulation
        switch (property)
        {
        case swmm_LINK_SETTING:
            return setLinkSetting(index, value);
		case swmm_LINK_FLOW_LIMIT:
			link->qLimit = value / UCF(FLOW);
			return 0;
		case swmm_LINK_SEEPAGE_RATE:
			if (value >= 0.0)
			{
				link->seepRate = value / UCF(RAINFALL);
				return 0;
			}
			else
				return ERR_API_PROPERTY_VALUE;
        case swmm_LINK_POLLUTANT_LATMASS_FLUX:
            link->apiExtQualMassFlux[subIndex] = value;
            return 0;
        default:
            return ERR_API_IS_RUNNING;
        }
    }
    else
    {  
        // Set values that can only be configured before simulation starts
        switch (property)
        {
        case swmm_LINK_SETTING:
            return setLinkSetting(index, value);
        case swmm_LINK_OFFSET1:
            Link[index].offset1 = value / UCF(LENGTH);
            return 0;
        case swmm_LINK_OFFSET2:
            Link[index].offset2 = value / UCF(LENGTH);
            return 0;
        case swmm_LINK_INITIAL_FLOW:
            Link[index].q0 = value / UCF(FLOW);
            return 0;
        case swmm_LINK_FLOW_LIMIT:
            Link[index].qLimit = value / UCF(FLOW);
            return 0;
        case swmm_LINK_INLET_LOSS:
            Link[index].cLossInlet = value;
            return 0;
        case swmm_LINK_OUTLET_LOSS:
            Link[index].cLossOutlet = value;
            return 0;
        case swmm_LINK_AVERAGE_LOSS:
            Link[index].cLossAvg = value;
            return 0;
        case swmm_LINK_SEEPAGE_RATE:
            if (value >= 0.0)
            {
                Link[index].seepRate = value / UCF(RAINFALL);
                return 0;
            }
            else
                return ERR_API_PROPERTY_VALUE;
            return 0;
        case swmm_LINK_HAS_FLAPGATE:
            Link[index].hasFlapGate = (value > 0.0);
            return 0;
        case swmm_LINK_POLLUTANT_LATMASS_FLUX:
            link->apiExtQualMassFlux[subIndex] = value;
            return 0;
        default:
            return ERR_API_PROPERTY_TYPE;
        }
    }
}

//=============================================================================

double DLLEXPORT swmm_getSavedValue(int property, int index, int period)
//
//  Input:   property = an object's property code
//           index = the object's index in the array of like objects
//           period = a reporting time period (starting from 1)
//  Output:  returns the property's saved value
//  Purpose: retrieves an object's computed value at a specific reporting time period.
{
    if (!IsOpenFlag)
        return 0;
    if (IsStartedFlag)
        return 0;
    if (period < 1 || period > Nperiods)
        return 0;
    if (property == swmm_CURRENTDATE)
        return getSavedDate(period);
    if (property >= 200 && property < 300)
        return getSavedSubcatchValue(property, index, period);
    if (property < 400)
        return getSavedNodeValue(property, index, period);
    if (property < 500)
        return getSavedLinkValue(property, index, period);
    return 0;
}

//=============================================================================

void DLLEXPORT swmm_decodeDate(double date, int *year, int *month, int *day,
                               int *hour, int *minute, int *second, int *dayOfWeek)
//
//  Input:  date = an encoded date in decimal days
//  Output: date's year, month of year, day of month, time of day (hour,
//           minute, second), and day of weeek
//  Purpose: retrieves the calendar date and clock time of an encoded date.
{
    datetime_decodeDate(date, year, month, day);
    datetime_decodeTime(date, hour, minute, second);
    *dayOfWeek = datetime_dayOfWeek(date);
}

//=============================================================================

double DLLEXPORT swmm_encodeDate(int year, int month, int day,
                                 int hour, int minute, int second)
//
//  Input:  date's year, month of year, day of month, time of day (hour,
//           minute, second), and day of weeek
//  Output: date = an encoded date in decimal days
//  Purpose: retrieves the calendar date and clock time of a decoded date.
{
    return datetime_encodeDate(year, month, day) + datetime_encodeTime(hour, minute, second);
}

//=============================================================================
//   Object property getters and setters
//=============================================================================

double getGageValue(int property, int index)
//
//  Input:   property = a rain gage property code
//           index = the index of a rain gage
//  Output:  returns current property value
//  Purpose: retrieves current value of a rain gage property.
{
    double total, snow, rain;

    if (index < 0 || index >= Nobjects[GAGE])
        return ERR_API_OBJECT_INDEX;

    total = gage_getPrecip(index, &rain, &snow);

    switch (property)
    {
    case swmm_GAGE_TOTAL_PRECIPITATION:
        return total * UCF(RAINFALL);
    case swmm_GAGE_RAINFALL:
        return rain * UCF(RAINFALL);
    case swmm_GAGE_SNOWFALL:
        return snow * UCF(RAINFALL);
    default:
        return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================

double getSubcatchValue(int property, int index, int subIndex)
//
//  Input:   property = a subcatchment property code
//           index = the index of a subcatchment
//           subIndex = an index of the subcatchment's property
//  Output:  returns current property value
//  Purpose: retrieves current value of a subcatchment's property.
{
    TSubcatch *subcatch;
    double prop_results = 0.0;
    int i;

    if (index < 0 || index >= Nobjects[SUBCATCH])
        return 0;

    subcatch = &Subcatch[index];

    switch (property)
    {
    case swmm_SUBCATCH_AREA:
        return subcatch->area * UCF(LANDAREA);
    case swmm_SUBCATCH_RAINGAGE:
        return subcatch->gage;
    case swmm_SUBCATCH_RAINFALL:
		return subcatch->rainfall * UCF(RAINFALL);
    case swmm_SUBCATCH_EVAP:
        return subcatch->evapLoss * UCF(EVAPRATE);
    case swmm_SUBCATCH_INFIL:
        return subcatch->infilLoss * UCF(RAINFALL);
    case swmm_SUBCATCH_RUNOFF:
        return subcatch->newRunoff * UCF(FLOW);
    case swmm_SUBCATCH_RPTFLAG:
        return (subcatch->rptFlag > 0);
    case swmm_SUBCATCH_WIDTH:
        return subcatch->width * UCF(LENGTH);
	case swmm_SUBCATCH_SLOPE:
		return subcatch->slope;
	case swmm_SUBCATCH_CURB_LENGTH:
		return subcatch->curbLength * UCF(LENGTH);
	case swmm_SUBCATCH_API_RAINFALL:
		return subcatch->apiRainfall * UCF(RAINFALL);
    case swmm_SUBCATCH_API_SNOWFALL:
		return subcatch->apiSnowfall * UCF(RAINFALL);
    case swmm_SUBCATCH_POLLUTANT_BUILDUP:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        
        for(i = 0; i < Nobjects[LANDUSE]; i++)
        {
            prop_results += subcatch->landFactor[i].buildup[subIndex] /
            (subcatch->area * UCF(LANDAREA) * subcatch->landFactor[i].fraction); 
        }

        return prop_results;
    case swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return subcatch->apiExtBuildup[subIndex] /  UCF(LANDAREA);

    case swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return subcatch->newQual[subIndex];

    case swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return subcatch->pondedQual[subIndex] / (subcatch_getDepth(index) * MAX(0.0, subcatch->area - subcatch->lidArea)  * UCF(LANDAREA));

    case swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return subcatch->totalLoad[subIndex];

    default:
        return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================

double getNodeValue(int property, int index, int subIndex)
//
//  Input:   property = a node property code
//           index = the index of a node
//           subIndex = an index of the node's property
//  Output:  returns current property value
//  Purpose: retrieves current value of a node's property.
{
    TNode *node;
    if (index < 0 || index >= Nobjects[NODE])
        return 0;
    else
    {
        node = &Node[index];
        
        switch (property)
        {
        case swmm_NODE_TYPE:
            return node->type;
        case swmm_NODE_ELEV:
            return node->invertElev * UCF(LENGTH);
        case swmm_NODE_MAXDEPTH:
            return node->fullDepth * UCF(LENGTH);
        case swmm_NODE_DEPTH:
            return node->newDepth * UCF(LENGTH);
        case swmm_NODE_HEAD:
            return (node->newDepth + node->invertElev) * UCF(LENGTH);
        case swmm_NODE_VOLUME:
            return node->newVolume * UCF(VOLUME);
        case swmm_NODE_LATFLOW:
            return node->newLatFlow * UCF(FLOW);
        case swmm_NODE_INFLOW:
            return node->inflow * UCF(FLOW);
        case swmm_NODE_OVERFLOW:
            return node->overflow * UCF(FLOW);
        case swmm_NODE_RPTFLAG:
            return (node->rptFlag > 0);
        case swmm_NODE_SURCHARGE_DEPTH:
            return node->surDepth * UCF(LENGTH);
        case swmm_NODE_PONDED_AREA:
            return node->pondedArea * UCF(LANDAREA);
        case swmm_NODE_INITIAL_DEPTH:
            return node->initDepth * UCF(LENGTH);
        case swmm_NODE_POLLUTANT_CONCENTRATION:
            if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
                return ERR_API_OBJECT_INDEX;
            return node->newQual[subIndex];
        case swmm_NODE_POLLUTANT_LATMASS_FLUX:
            if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
                return ERR_API_OBJECT_INDEX;
            return node->apiExtQualMassFlux[subIndex];
        default:
            return ERR_API_OBJECT_TYPE;
        }
    }
}

//=============================================================================

double getLinkValue(int property, int index, int subIndex)
//
//  Input:   property = a link property code
//           index = the index of a link
//           subIndex = an index of the link's property
//  Output:  returns current property value
//  Purpose: retrieves current value of a link's property.
{
    TLink *link;
    if (index < 0 || index >= Nobjects[LINK])
        return 0;
    link = &Link[index];
    switch (property)
    {
    case swmm_LINK_TYPE:
        return link->type;
    case swmm_LINK_NODE1:
        return link->node1;
    case swmm_LINK_NODE2:
        return link->node2;
    case swmm_LINK_LENGTH:
        if (link->type == CONDUIT)
            return Conduit[link->subIndex].length * UCF(LENGTH);
        else
            return ERR_API_OBJECT_TYPE;
    case swmm_LINK_SLOPE:
        if (link->type == CONDUIT)
            return Conduit[link->subIndex].slope;
        else
            return ERR_API_OBJECT_TYPE;
        break;
    case swmm_LINK_FULLDEPTH:
        return link->xsect.yFull * UCF(LENGTH);
    case swmm_LINK_FULLFLOW:
        return link->qFull * UCF(FLOW);
    case swmm_LINK_SETTING:
        return link->setting;
    case swmm_LINK_TIMEOPEN:
        if (link->setting > 0.0)
            return (getDateTime(NewRoutingTime) - link->timeLastSet) * 24.;
        else
            return 0;
    case swmm_LINK_TIMECLOSED:
          if (link->setting <= 0.0)
              return (getDateTime(NewRoutingTime) - link->timeLastSet) * 24.;
          else
              return 0;
    case swmm_LINK_FLOW:
        return link->newFlow * UCF(FLOW);
    case swmm_LINK_DEPTH:
        return link->newDepth * UCF(LENGTH);
    case swmm_LINK_VELOCITY:
        return link_getVelocity(index, fabs(link->newFlow), link->newDepth) * UCF(LENGTH);
    case swmm_LINK_TOPWIDTH:
        if (link->type == CONDUIT)
            return xsect_getWofY(&link->xsect, link->newDepth) * UCF(LENGTH);
        else
            return ERR_API_OBJECT_TYPE;
    case swmm_LINK_RPTFLAG:
        return (link->rptFlag > 0);
	case swmm_LINK_OFFSET1:
		return link->offset1 * UCF(LENGTH);
	case swmm_LINK_OFFSET2:
		return link->offset2 * UCF(LENGTH);
	case swmm_LINK_INITIAL_FLOW:
		return link->q0 * UCF(FLOW);
	case swmm_LINK_FLOW_LIMIT:
		return link->qLimit * UCF(FLOW);
	case swmm_LINK_INLET_LOSS:
		return link->cLossInlet;
	case swmm_LINK_OUTLET_LOSS:
		return link->cLossOutlet;
	case swmm_LINK_AVERAGE_LOSS:
		return link->cLossAvg;
	case swmm_LINK_SEEPAGE_RATE:
		return link->seepRate * UCF(RAINFALL);
	case swmm_LINK_HAS_FLAPGATE:
		return (link->hasFlapGate > 0);
    case swmm_LINK_POLLUTANT_CONCENTRATION:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return link->newQual[subIndex];
    case swmm_LINK_POLLUTANT_LOAD:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return link->totalLoad[subIndex];
    case swmm_LINK_POLLUTANT_LATMASS_FLUX:
        if (subIndex < 0 || subIndex >= Nobjects[POLLUT])
            return ERR_API_OBJECT_INDEX;
        return link->apiExtQualMassFlux[subIndex];
    default:
        return ERR_API_OBJECT_TYPE;
    }
}

//=============================================================================

double getSystemValue(int property)
//
//  Input:   property = a system property code
//  Output:  returns current property value or an error code
//  Purpose: retrieves current value of a system property.
{
    switch (property)
    {
    case swmm_STARTDATE:
        return StartDateTime;
    case swmm_CURRENTDATE:
        return StartDateTime + ElapsedTime;
    case swmm_ELAPSEDTIME:
        return ElapsedTime;
    case swmm_ROUTESTEP:
        return RouteStep;
    case swmm_MAXROUTESTEP:
        return getMaxRouteStep();
    case swmm_REPORTSTEP:
        return ReportStep;
    case swmm_TOTALSTEPS:
        return Nperiods;
    case swmm_NOREPORT:
        return RptFlags.disabled;
    case swmm_FLOWUNITS:
        return FlowUnits;
    case swmm_ENDDATE:
        return EndDateTime;
    case swmm_REPORTSTART:
        return ReportStart;
    case swmm_UNITSYSTEM:
        return UnitSystem;
    case swmm_SURCHARGEMETHOD:
        return SurchargeMethod;
    case swmm_ALLOWPONDING:
        return AllowPonding;
    case swmm_INERTIADAMPING:
        return InertDamping;
    case swmm_NORMALFLOWLTD:
        return NormalFlowLtd;
    case swmm_SKIPSTEADYSTATE:
        return SkipSteadyState;
    case swmm_IGNORERAINFALL:
        return IgnoreRainfall;
    case swmm_IGNORERDII:
        return IgnoreRDII;
    case swmm_IGNORESNOWMELT:
        return IgnoreSnowmelt;
    case swmm_IGNOREGROUNDWATER:
        return IgnoreGwater;
    case swmm_IGNOREROUTING:
        return IgnoreRouting;
    case swmm_IGNOREQUALITY:
        return IgnoreQuality;
    case swmm_ERROR_CODE:
        return ErrorCode;
    case swmm_RULESTEP:
        return RuleStep;
    case swmm_SWEEPSTART:
        return SweepStart;
    case swmm_SWEEPEND:
        return SweepEnd;
    case swmm_MAXTRIALS:
        return MaxTrials;
    case swmm_NUMTHREADS:
        return NumThreads;
    case swmm_MINROUTESTEP:
        return MinRouteStep;
    case swmm_LENGTHENINGSTEP:
        return LengtheningStep;
    case swmm_STARTDRYDAYS:
        return StartDryDays;
    case swmm_COURANTFACTOR:
        return CourantFactor;
    case swmm_MINSURFAREA:
        return MinSurfArea * UCF(LENGTH) * UCF(LENGTH);
    case swmm_MINSLOPE:
        return MinSlope;
    case swmm_RUNOFFERROR:
        return RunoffError;
    case swmm_FLOWERROR:
        return FlowError;
    case swmm_HEADTOL:
        return HeadTol * UCF(LENGTH);
    case swmm_SYSFLOWTOL:
        return SysFlowTol;
    case swmm_LATFLOWTOL:
        return LatFlowTol;
    default:
        return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================

int setNodeLatFlow(int index, double value)
//
//  Input:   index = the index of a node
//           value = the node's external inflow value
//  Output:  returns an error code
//  Purpose: sets the value of a node's external inflow.
{
    if (index < 0 || index >= Nobjects[NODE])
        return ERR_API_OBJECT_INDEX;
    Node[index].apiExtInflow = value / UCF(FLOW);
    return 0;
}

//=============================================================================

int setOutfallStage(int index, double value)
//
//  Input:   index = the index of an outfall node
//           value = the outfall's fixed stage elevation
//  Output:  returns an error code
//  Purpose: sets the value of an outfall node's fixed stage.
{
    TNode *node;
    if (index < 0 || index >= Nobjects[NODE])
        return ERR_API_OBJECT_INDEX;

    node = &Node[index];

    if (node->type != OUTFALL)
        return ERR_API_OBJECT_TYPE;

    Outfall[node->subIndex].fixedStage = value / UCF(LENGTH);
    Outfall[node->subIndex].type = FIXED_OUTFALL;

    return 0;
}

//=============================================================================

int setLinkSetting(int index, double value)
//
//  Input:   index = the index of a link
//           value = the link's new setting
//  Output:  returns an error code
//  Purpose: sets the value of a link's setting.
{
    TLink *link;
    char control_rule_label[9] = "SWMM API";
    double currentTime;

    if (index < 0 || index >= Nobjects[LINK])
        return ERR_API_OBJECT_INDEX;

    link = &Link[index];
    if (value < 0.0 || link->type == CONDUIT)
        return ERR_API_OBJECT_INDEX;

    if (link->type != PUMP && value > 1.0)
        value = 1.0;

    if (link->targetSetting == value)
        return 0;

    link->targetSetting = value;

    if (link->targetSetting * link->setting == 0.0)
        link->timeLastSet = StartDateTime + ElapsedTime;

    link_setSetting(index, 0.0);

    // Add control action to RPT file if desired flagged
    if (RptFlags.controls)
    {
        currentTime = getDateTime(NewRoutingTime);
        report_writeControlAction(currentTime, Link[index].ID, value, control_rule_label);
    }

    return 0;
}

//=============================================================================

double getSavedDate(int period)
//
//  Input:   period = a reporting period (starting at 1)
//  Output:  returns the date/time of the reporting period in decimal days
//  Purpose: retrieves the date/time of a reporting period.
{
    double days;
    output_readDateTime(period, &days);
    return days;
}

//=============================================================================

double getSavedSubcatchValue(int property, int index, int period)
//
//  Input:   property = index of a computed property
//           index = index of a subcatchment
//           period = a reporting period (starting at 1)
//  Output:  returns the property's value at the recording period
//  Purpose: retrieves the computed value of a subcatchment property at a
//           specific reporting period.
{
    // --- SubcatchResults array is defined in output.c and contains
    //     computed results in user's units
    extern float *SubcatchResults;

    // --- order in which subcatchment was saved to output results file
    int outIndex = Subcatch[index].rptFlag - 1;
    if (outIndex < 0)
        return 0;

    output_readSubcatchResults(period, outIndex);
    switch (property)
    {
    case swmm_SUBCATCH_RAINFALL:
        return SubcatchResults[SUBCATCH_RAINFALL];
    case swmm_SUBCATCH_EVAP:
        return SubcatchResults[SUBCATCH_EVAP];
    case swmm_SUBCATCH_INFIL:
        return SubcatchResults[SUBCATCH_INFIL];
    case swmm_SUBCATCH_RUNOFF:
        return SubcatchResults[SUBCATCH_RUNOFF];
    default:
        return 0;
    }
}

//=============================================================================

double getSavedNodeValue(int property, int index, int period)
//
//  Input:   property = index of a computed property
//           index = index of a node
//           period = a reporting period (starting at 1)
//  Output:  returns the property's value at the recording period
//  Purpose: retrieves the computed value of a node property at a
//           specific reporting period.
{
    // --- NodeResults array is defined in output.c and contains
    //     computed results in user's units
    extern float *NodeResults;

    // --- order in which node was saved to output results file
    int outIndex = Node[index].rptFlag - 1;
    if (outIndex < 0)
        return 0;

    output_readNodeResults(period, outIndex);
    switch (property)
    {
    case swmm_NODE_DEPTH:
        return NodeResults[NODE_DEPTH];
    case swmm_NODE_HEAD:
        return NodeResults[NODE_HEAD];
    case swmm_NODE_VOLUME:
        return NodeResults[NODE_VOLUME];
    case swmm_NODE_LATFLOW:
        return NodeResults[NODE_LATFLOW];
    case swmm_NODE_INFLOW:
        return NodeResults[NODE_INFLOW];
    case swmm_NODE_OVERFLOW:
        return NodeResults[NODE_OVERFLOW];
    default:
        return 0;
    }
}

//=============================================================================

double getSavedLinkValue(int property, int index, int period)
//
//  Input:   property = index of a computed property
//           index = index of a link
//           period = a reporting period (starting at 1)
//  Output:  returns the property's value at the recording period
//  Purpose: retrieves the computed value of a link property at a
//           specific reporting period.
{
    double y, w;

    // --- LinkResults array is defined in output.c and contains
    //     computed results in user's units
    extern float *LinkResults;

    // --- order in which link was saved to output results file
    int outIndex = Link[index].rptFlag - 1;
    if (outIndex < 0)
        return 0;

    output_readLinkResults(period, outIndex);
    switch (property)
    {
    case swmm_LINK_FLOW:
        return LinkResults[LINK_FLOW];
    case swmm_LINK_DEPTH:
        return LinkResults[LINK_DEPTH];
    case swmm_LINK_VELOCITY:
        return LinkResults[LINK_VELOCITY];
    case swmm_LINK_TOPWIDTH:
        y = LinkResults[LINK_DEPTH] / UCF(LENGTH);
        w = xsect_getWofY(&Link[index].xsect, y);
        return w * UCF(LENGTH);
    case swmm_LINK_SETTING:
        return LinkResults[LINK_CAPACITY];
    default:
        return 0;
    }
}

//=============================================================================

double getMaxRouteStep()
{
    double tmpCourantFactor = CourantFactor;
    double result = RouteStep;

    if (!IsStartedFlag || RouteModel != DW)
        return result;
    CourantFactor = 1.0;
    result = routing_getRoutingStep(RouteModel, MinRouteStep);
    CourantFactor = tmpCourantFactor;
    return result;
}

//=============================================================================

int setRoutingStep(double value)
//
//  Input:   value = a routing time step (in decimal seconds)
//  Output:  returns an error code
//  Purpose: sets the value of the current flow routing time step.
{
    if (value <= 0.0)
        return ERR_API_PROPERTY_VALUE;

    if (value <= MinRouteStep)
        value = MinRouteStep;

    CourantFactor = 0.0;
    RouteStep = value;

    return 0;
}

//=============================================================================

int setSystemValue(int property, double value)
//
//  Input:   property = a system property code
//           value = the property's new value
//  Output:  returns an error code
//  Purpose: sets the value of a system property.
{
    int y, m, d, h, mm, s;

    if (IsStartedFlag)
        return ERR_API_NOT_ENDED;

    switch (property)
    {
    case swmm_STARTDATE:
        StartDateTime = value;
        datetime_decodeDate(value, &y, &m, &d);
        datetime_decodeTime(value, &h, &mm, &s);
        StartDate = datetime_encodeDate(y, m, d);
        StartTime = datetime_encodeTime(h, mm, s);
        TotalDuration = floor((EndDate - StartDate) * SECperDAY + (EndTime - StartTime) * SECperDAY);
        // convert total duration to milliseconds
        TotalDuration *= 1000.0;
        return 0;
    case swmm_ROUTESTEP:
        return setRoutingStep(value);

    case swmm_REPORTSTEP:
        if (value > 0)
        {
            ReportStep = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }

    case swmm_NOREPORT:
        RptFlags.disabled = (value > 0.0);
        return 0;

    case swmm_ENDDATE:
        EndDateTime = value;
        datetime_decodeDate(value, &y, &m, &d);
        datetime_decodeTime(value, &h, &mm, &s);
        EndDate = datetime_encodeDate(y, m, d);
        EndTime = datetime_encodeTime(h, mm, s);
        TotalDuration = floor((EndDate - StartDate) * SECperDAY + (EndTime - StartTime) * SECperDAY);
        // convert total duration to milliseconds
        TotalDuration *= 1000.0;
        return 0;
    case swmm_REPORTSTART:
        ReportStart = value;
        datetime_decodeDate(value, &y, &m, &d);
        datetime_decodeTime(value, &h, &mm, &s);
        ReportStartDate = datetime_encodeDate(y, m, d);
        ReportStartTime = datetime_encodeTime(h, mm, s);
        return 0;
    case swmm_NUMTHREADS:
        // possible over allocation of threads but we trust the user to know what they are doing. Limit to max threads.
        NumThreads = MAX(1, MIN((int)value, omp_get_max_threads()));
        return 0;
    case swmm_SURCHARGEMETHOD:
        if (value >= EXTRAN && value <= SLOT)
        {
            SurchargeMethod = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_ALLOWPONDING:
        AllowPonding = (value > 0.0);
        return 0;
    case swmm_INERTIADAMPING:
        if (value >= NO_DAMPING && value <= FULL_DAMPING)
        {
            InertDamping = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_NORMALFLOWLTD:
        if (value >= SLOPE && value <= NEITHER)
        {
            NormalFlowLtd = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_SKIPSTEADYSTATE:
        SkipSteadyState = (value > 0.0);
        return 0;
    case swmm_IGNORERAINFALL:
        IgnoreRainfall = (value > 0.0);
        return 0;
    case swmm_IGNORERDII:
        IgnoreRDII = (value > 0.0);
        return 0;
    case swmm_IGNORESNOWMELT:
        IgnoreSnowmelt = (value > 0.0);
        return 0;
    case swmm_IGNOREGROUNDWATER:
        IgnoreGwater = (value > 0.0);
        return 0;
    case swmm_IGNOREROUTING:
        IgnoreRouting = (value > 0.0);
        return 0;
    case swmm_IGNOREQUALITY:
        IgnoreQuality = (value > 0.0);
        return 0;
    case swmm_RULESTEP:
        if (value > 0)
        {
            RuleStep = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_SWEEPSTART:
        if (value >= 0.0 && value <= 365)
        {
            SweepStart = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_SWEEPEND:
        if (value >= 0.0 && value <= 365)
        {
            SweepEnd = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_MAXTRIALS:
        if (value >= 2)
        {
            MaxTrials = (int)value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_MINROUTESTEP:
        if (value > 0)
        {
            MinRouteStep = value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_LENGTHENINGSTEP:
        if (value > 0)
        {
            LengtheningStep = value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_STARTDRYDAYS:
        if (value >= 0)
        {
            StartDryDays = value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_COURANTFACTOR:
        if (value > 0 && value <= 2.0)
        {
            CourantFactor = value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_MINSURFAREA:
        if (value >= 0)
        {
            MinSurfArea = value / UCF(LENGTH) / UCF(LENGTH);
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    case swmm_MINSLOPE:
        if (value >= 0 && value < 100) 
        {
            MinSlope = value;
            return 0;
        }
        else
        {
            return ERR_API_PROPERTY_VALUE;
        }
    default:
        return ERR_API_PROPERTY_TYPE;
    }
}

//=============================================================================
//   General purpose functions
//=============================================================================

double UCF(int u)
//
//  Input:   u = integer code of quantity being converted
//  Output:  returns a units conversion factor
//  Purpose: computes a conversion factor from SWMM's internal
//           units to user's units
//
{
    if (u < FLOW)
        return Ucf[u][UnitSystem];
    else
        return Qcf[FlowUnits];
}

//=============================================================================

size_t sstrncpy(char *dest, const char *src, size_t n)
//
//  Input:   dest = string to be copied to
//           src = string to be copied from
//           n = number of bytes to copy
//  Output:  returns the size of dest
//  Purpose: better version of standard strncpy function
//
{
    int offset = 0;
    if (n > 0)
    {
        while (*(src + offset) != '\0')
        {
            if (offset == n)
                break;
            *(dest + offset) = *(src + offset);
            offset++;
        }
    }
    *(dest + offset) = '\0';
    return strlen(dest);
}

//=============================================================================

size_t sstrcat(char *dest, const char *src, size_t size)
//
//  Input:   dest = string to be appended
//           src = string to append to dest
//           size = allocated size of dest (including nul terminator)
//  Output:  returns new size of dest
//  Purpose: safe version of standard strcat function
//
{
    size_t dest_len, src_len, offset, src_index;

    // obtain initial sizes
    dest_len = strlen(dest);
    src_len = strlen(src);

    // get the end of dest
    offset = dest_len;

    // append src
    src_index = 0;
    while (*(src + src_index) != '\0')
    {
        *(dest + offset) = *(src + src_index);
        offset++;
        src_index++;
        // don't copy more than size - dest_len - 1 characters
        if (offset == size - 1)
            break;
    }
    *(dest + offset) = '\0';
    return strlen(dest);
}

//=============================================================================

int strcomp(const char *s1, const char *s2)
//
//  Input:   s1 = a character string
//           s2 = a character string
//  Output:  returns 1 if s1 is same as s2, 0 otherwise
//  Purpose: does a case insensitive comparison of two strings.
//
{
    int i;
    for (i = 0; UCHAR(s1[i]) == UCHAR(s2[i]); i++)
    {
        if (!s1[i + 1] && !s2[i + 1])
            return (1);
    }
    return (0);
}

//=============================================================================

char *getTempFileName(char *fname)
//
//  Input:   fname = file name string (with max size of MAXFNAME)
//  Output:  returns pointer to file name
//  Purpose: creates a temporary file name with path prepended to it.
//
{
// For Windows systems:
#ifdef WINDOWS

    char *name = NULL;
    char *dir = NULL;

    // --- set dir to user's choice of a temporary directory
    if (strlen(TempDir) > 0)
    {
        if (_mkdir(TempDir) == 0 || errno == EEXIST)
            dir = TempDir;
    }

    // --- use _tempnam to get a pointer to an unused file name
    name = _tempnam(dir, "swmm");
    if (name == NULL)
        return NULL;

    // --- copy the file name to fname
    if (strlen(name) <= MAXFNAME)
        sstrncpy(fname, name, MAXFNAME);
    else
        fname = NULL;

    // --- free the pointer returned by _tempnam
    free(name);

    // --- return the new contents of fname
    return fname;

// For non-Windows systems:
#else

    // --- use system function mkstemp() to create a temporary file name
    sstrncpy(fname, "swmmXXXXXX", MAXFNAME);
    mkstemp(fname);
    return fname;

#endif
}

//=============================================================================

void getElapsedTime(DateTime aDate, int *days, int *hrs, int *mins)
//
//  Input:   aDate = simulation calendar date + time
//  Output:  days, hrs, mins = elapsed days, hours & minutes for aDate
//  Purpose: finds elapsed simulation time for a given calendar date
//
{
    DateTime x;
    int secs;
    x = aDate - ReportStart;
    if (x <= 0.0)
    {
        *days = 0;
        *hrs = 0;
        *mins = 0;
    }
    else
    {
        *days = (int)x;
        datetime_decodeTime(x, hrs, mins, &secs);
    }
}

//=============================================================================

DateTime getDateTime(double elapsedMsec)
//
//  Input:   elapsedMsec = elapsed milliseconds
//  Output:  returns date/time value
//  Purpose: finds calendar date/time value for elapsed milliseconds of
//           simulation time.
//
{
    return datetime_addSeconds(StartDateTime, (elapsedMsec + 1) / 1000.0);
}

//=============================================================================

static int isRelativePath(const char *fname)
//
//  Input:   fname = a file name
//  Output:  returns 1 if fname's path is relative or 0 if absolute
//  Purpose: determines if a file name contains a relative or absolute path.
//
{
    if (strchr(fname, ':'))
        return 0;
    if (fname[0] == '\\')
        return 0;
    if (fname[0] == '/')
        return 0;
    return 1;
}

//=============================================================================

void getAbsolutePath(const char *fname, char *absPath, size_t size)
//
//  Input:   fname = a file name
//           absPath = string to hold the absolute path
//           size = max. size of absPath
//  Output:  absPath = string containing absolute path of fname
//                     (including ending path delimiter)
//  Purpose: finds the full path of the directory for file fname
//
{
    char *endOfDir;

    // --- case of empty file anme
    if (fname == NULL || strlen(fname) == 0)
        return;

    // --- if fname has a relative path then retrieve its full path
    if (isRelativePath(fname))
    {
#ifdef WINDOWS
        GetFullPathName((LPCSTR)fname, (DWORD)size, (LPSTR)absPath, NULL);
#else
        realpath(fname, absPath);
#endif
    }

    // --- otherwise copy fname to absPath
    else
    {
        sstrncpy(absPath, fname, strlen(fname));
    }

// --- trim file name portion of absPath
#ifdef WINDOWS
    endOfDir = strrchr(absPath, '\\');
#else
    endOfDir = strrchr(absPath, '/');
#endif
    if (endOfDir)
    {
        *(endOfDir + 1) = '\0';
    }
}

//=============================================================================

char *addAbsolutePath(char *fname)
//
//  Input:   fname = a file name
//  Output:  returns fname with a full path prepended to it
//  Purpose: adds an absolute path name to a file name.
//  Note:    fname must have been dimensioned to accept MAXFNAME characters.
//
{
    size_t n;
    char buffer[MAXFNAME];
    if (isRelativePath(fname))
    {
        n = snprintf(buffer, MAXFNAME, "%s%s", InpDir, fname);
        if (n > 0)
            sstrncpy(fname, buffer, MAXFNAME);
    }
    return fname;
}

//=============================================================================

void writecon(const char *s)
//
//  Input:   s = a character string
//  Output:  none
//  Purpose: writes string of characters to the console.
//
{
    fprintf(stdout, "%s", s);
    fflush(stdout);
}

//=============================================================================

#ifdef EXH
int xfilter(int xc, char *module, double elapsedTime, long step)
//
//  Input:   xc          = exception code
//           module      = name of code module where exception was handled
//           elapsedTime = simulation time when exception occurred (days)
//           step        = step count at time when exception occurred
//  Output:  returns an exception handling code
//  Purpose: exception filtering routine for operating system exceptions
//           under Windows and the Microsoft C compiler.
//
{
    int rc;         // result code
    long hour;      // current hour of simulation
    char msg[40];   // exception type text
    char xmsg[240]; // error message text
    switch (xc)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        sprintf(msg, "\n  Access violation ");
        rc = EXCEPTION_EXECUTE_HANDLER;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        sprintf(msg, "\n  Illegal floating point operand ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        sprintf(msg, "\n  Floating point divide by zero ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        sprintf(msg, "\n  Illegal floating point operation ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        sprintf(msg, "\n  Floating point overflow ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        sprintf(msg, "\n  Floating point stack violation ");
        rc = EXCEPTION_EXECUTE_HANDLER;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        sprintf(msg, "\n  Floating point underflow ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        sprintf(msg, "\n  Integer divide by zero ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    case EXCEPTION_INT_OVERFLOW:
        sprintf(msg, "\n  Integer overflow ");
        rc = EXCEPTION_CONTINUE_EXECUTION;
        break;
    default:
        sprintf(msg, "\n  Exception %d ", xc);
        rc = EXCEPTION_EXECUTE_HANDLER;
    }
    hour = (long)(elapsedTime / 1000.0 / 3600.0);
    sprintf(xmsg, "%sin module %s at step %ld, hour %ld",
            msg, module, step, hour);
    if (rc == EXCEPTION_EXECUTE_HANDLER ||
        ++ExceptionCount >= MAX_EXCEPTIONS)
    {
        strcat(xmsg, " --- execution halted.");
        rc = EXCEPTION_EXECUTE_HANDLER;
    }
    report_writeLine(xmsg);
    return rc;
}
#endif
