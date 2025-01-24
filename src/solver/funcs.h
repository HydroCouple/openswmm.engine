/*!
 * \file funcs.h
 * \brief Header file for global interfacing functions.
 * \author L. Rossman
 * \author M. Tryby (EPA)
 * \date Created: 2023-06-12
 * \date Last updated: 2025-01-02
 * \version 5.3.0
 * \details
 * Update History
 * =================
 * Global interfacing functions used in the SWMM model.
 * Build 5.1.007:
 * - climate_readAdjustments() added.
 * Build 5.1.008:
 * - Function list was re-ordered and blank lines added for readability.
 * - Pollutant buildup/washoff functions for the new surfqual.c module added.
 * - Several other functions added, re-named or have modified arguments.
 * Build 5.1.010:
 * - New roadway_getInflow() function added.
 * Build 5.1.013:
 * - Additional arguments added to function stats_updateSubcatchStats.
 * Build 5.1.014:
 * - Arguments to link_getLossRate function changed.
 * Build 5.2.0:
 * - Support added for Streets and Inlets.
 * - Support added for reporting most frequent non-converging links.
 * - Support added for named variables & math expressions in control rules.
 * - Support added for tracking a gage's prior n-hour rainfall total.
 * - Refactored external inflow code.
 * Build 5.2.4:
 * - Additional arguments added to function link_getLossRate.
 * Build 5.3.0:
 * - Modified code to allow saving multiple hotstart files at different times.
 */

#ifndef FUNCS_H
#define FUNCS_H

/*!
 * \defgroup Global_Interfacing_Functions SWMM Global Interfacing Functions
 * \brief SWMM global interfacing functions
 * \{
 */

/*!
 * \addtogroup Project_Methods Global Project Methods
 * \brief Global methods associated with a SWMM project
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Opens a new SWMM project
 * \param[in] inp_file SWMM input file
 * \param[in] rpt_file SWMM report file
 * \param[in] out_file SWMM output file
 */
void project_open(const char *inp_file, const char *rpt_file, const char *out_file);

/*!
 * \brief Close project
 */
void project_close(void);

/*!
 * \brief Read project inputs
 */
void project_readInput(void);

/*!
 * \brief Reads a project option from a pair of string tokens
 * \param[in] s1 Option keyword
 * \param[in] s2 String representation of option's value
 * \return Error code
 * \note All project options have default values assigned in setDefaults().
 */
int project_readOption(char *s1, char *s2);

/*!
 * \brief Checks validity of project data
 */
void project_validate(void);

/*!
 * \brief Initializes the internal state of the project
 */
int project_init(void);

/*!
 * \brief Adds an object Id to a hash table
 * \param[in] type Object type
 * \param[in] id Object ID string
 * \param[in] n Object index
 * \return 0 if object already added, 1 if not, -1 if hashing fails
 */
int project_addObject(int type, char *id, int n);

/*!
 * \brief Uses hash table to find index of an object with a given ID
 * \param[in] type Object type
 * \param[in] id Object ID string
 * \return Returns index of object with given ID, or -1 if not found
 */
int project_findObject(int type, const char *id);

/*!
 * \brief Uses hash table to find address of a given string entry
 * \param[in] type Object type
 * \param[in] id ID name being sought
 * \return Pointer to location where object's ID string is stored
 */
char *project_findID(int type, char *id);

/*!
 * \brief Allocates memory for a matrix of doubles
 * \param[in] nrows Number of rows (0-based)
 * \param[in] ncols Number of columns (0-based)
 * \return Pointer to a matrix of doubles
 */
double **project_createMatrix(int nrows, int ncols);

/*!
 * \brief Frees memory allocated for a matrix of doubles
 */
void project_freeMatrix(double **m);
/*!
 * \}
 */

/*!
 * \addtogroup Input_Reader_Methods Global Input Reader Methods
 * \brief Global input reader methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Reads input file to determine number of system objects
 * \return Error code
 */
int input_countObjects(void);

/*!
 * \brief Reads input file to determine number of system objects
 * \return Error code
 */
int input_readData(void);
/*!
 * \}
 */

/*!
 * \addtogroup Report_Writer_Methods Global Report Writer Methods
 * \brief Global report writer methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Reads reporting options from a line of input data
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 */
int report_readOptions(char *tok[], int ntoks);

/*!
 * \brief Writes a line of text to the report file
 * \param[in] line Line of text
 */
void report_writeLine(const char *line);

/*!
 * \brief Writes starting/ending processing times to the report file
 */
void report_writeSysTime(void);

/*!
 * \brief Writes report header lines to report file
 */
void report_writeLogo(void);

/*!
 * \brief Writes project title to report file
 */
void report_writeTitle(void);

/*!
 * \brief Writes analysis options in use to report file
 */
void report_writeOptions(void);

/*!
 * \brief Writes project title to report file
 */
void report_writeReport(void);

/*!
 * \brief Writes summary of rain data read from input file to report file
 * \param[in] gage Rain gage index
 * \param[in] rainStats Rain file summary statistics
 */
void report_writeRainStats(int gage, TRainStats *rainStats);

/*!
 * \brief Writes summary of RDII inflow to report file
 * \param[in] totalRain Total rainfall volume over sewershed
 * \param[in] totalRdii Total RDII volume produced
 */
void report_writeRdiiStats(double totalRain, double totalRdii);

/*!
 * \brief Writes summary control actions heading to report file
 */
void report_writeControlActionsHeading(void);

/*!
 * \brief Writes control action to report file
 * \param[in] aDate Date/time of rule action
 * \param[in] linkID ID of link being controlled
 * \param[in] value New status value of link
 * \param[in] ruleID ID of rule implementing the action
 */
void report_writeControlAction(DateTime aDate, char *linkID, double value,
                               char *ruleID);

/*!
 * \brief Writes runoff continuity error to report file
 * \param[in] totals Accumulated runoff totals
 * \param[in] totalArea Total area of all subcatchments
 */
void report_writeRunoffError(TRunoffTotals *totals, double area);

/*!
 * \brief Writes runoff loading continuity error to report file
 * \param[in] totals Accumulated pollutant loading totals
 */
void report_writeLoadingError(TLoadingTotals *totals);

/*!
 * \brief Writes groundwater continuity error to report file
 * \param[in] totals Accumulated groundwater totals
 * \param[in] area Total area of all subcatchments with groundwater
 */
void report_writeGwaterError(TGwaterTotals *totals, double area);

/*!
 * \brief Writes flow routing continuity error to report file
 * \param[in] totals Accumulated flow routing totals
 */
void report_writeFlowError(TRoutingTotals *totals);

/*!
 * \brief Writes quality routing continuity error to report file
 * \param[in] QualTotals Accumulated quality routing totals for each pollutant
 */
void report_writeQualError(TRoutingTotals *totals);

/*!
 * \brief Lists nodes and links with highest mass balance errors and courant time
 *        step violations to report file
 * \param[in] massBalErrs Nodes with highest mass balance errors
 * \param[in] CourantCrit Nodes most often Courant time step critical
 * \param[in] nMaxStats Number of most critical nodes/links saved
 */
void report_writeMaxStats(TMaxStats massBalErrs[], TMaxStats CourantCrit[],
                          int nMaxStats);

/*!
 * \brief Lists links with highest number of flow turns to report (i.e., fraction
 *        of time periods where the flow is higher (or lower) than the
 *        flows in the previous and following periods)
 * \param[in] flowTurns Links with highest number of flow turns
 * \param[in] nMaxStats Number of links in flowTurns[]
 */
void report_writeMaxFlowTurns(TMaxStats flowTurns[], int nMaxStats);

/*!
 * \brief Writes non-converging nodes to report file
 * \param[in] maxNonconverged Nodes most often Courant time step critical
 * \param[in] nMaxStats Number of most critical nodes/links saved
 */
void report_writeNonconvergedStats(TMaxStats maxNonconverged[],
                                   int nMaxStats);

/*!
 * \brief Writes routing time step statistics to report file
 * \param[in] timeStepStats Routing time step statistics
 */
void report_writeTimeStepStats(TTimeStepStats *timeStepStats);

/*!
 * \brief Writes error message to report file
 * \param[in] code Error code
 * \param[in] msg Error message text
 */
void report_writeErrorMsg(int code, char *msg);

/*!
 * \brief Writes error message to report file
 */
void report_writeErrorCode(void);

/*!
 * \brief Writes input error message to report file
 * \param[in] k Error code
 * \param[in] sect Number of input data section where error occurred
 * \param[in] line Line of data containing the error
 * \param[in] lineCount Line number of data file containing the error
 */
void report_writeInputErrorMsg(int k, int sect, char *line, long lineCount);

/*!
 * \brief Writes a warning message to report file
 * \param[in] msg Text of warning message
 * \param[in] id ID name of object that message refers to
 */
void report_writeWarningMsg(char *msg, char *id);

/*!
 * \brief Writes the date where a time series' data is out of order
 * \param[in] code Error code
 * \param[in] tseries Pointer to a time series
 */
void report_writeTseriesErrorMsg(int code, TTable *tseries);

/*!
 * \brief Writes summary of input data to report file
 */
void inputrpt_writeInput(void);

/*!
 * \brief Reports simulation summary statistics
 */
void statsrpt_writeReport(void);
/*!
 * \}
 */

/*!
 * \addtogroup Climate_Methods Global Temperature/Evaporation Methods
 * \brief Global temperature/evaporation methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
 * \brief Reads climate/temperature parameters from input line of data
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details
 * Format of data can be:
 * - TIMESERIES  name
 * - FILE        name  (start)  (units)
 * - WINDSPEED   MONTHLY  v1  v2  ...  v12
 * - WINDSPEED   FILE
 * - SNOWMELT    v1  v2  ...  v6
 * - ADC         IMPERV/PERV  v1  v2  ...  v10
 */
int climate_readParams(char *tok[], int ntoks);

/*!
 * \brief Reads evaporation parameters from input line of data.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details
 * Data formats are:
 * - CONSTANT  value
 * - MONTHLY   v1 ... v12
 * - TIMESERIES name
 * - TEMPERATURE
 * - FILE      (v1 ... v12)
 * - RECOVERY   name
 * - DRY_ONLY   YES/NO
 */
int climate_readEvapParams(char *tok[], int ntoks);

/*!
 * \brief Reads adjustments to monthly evaporation or rainfall from input line of data.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details
 * Data formats are:
 * - TEMPERATURE   v1 ... v12
 * - EVAPORATION   v1 ... v12
 * - RAINFALL      v1 ... v12
 * - CONDUCTIVITY  v1 ... v12
 * - N-PERV        subcatchID  patternID
 * - DSTORE        subcatchID  patternID
 * - INFIL         subcatchID  patternID
 */
int climate_readAdjustments(char *tok[], int ntoks);

/*!
 * \brief Validates climatological variables.
 */
void climate_validate(void);

/*!
 * \brief Opens a climate file and reads in first set of values.
 */
void climate_openFile(void);

/*!
 * \brief Initializes climate state variables.
 */
void climate_initState(void);

/*!
 * \brief Sets climate variables for current date.
 * \param[in] theDate Simulation date
 */
void climate_setState(DateTime theDate);

/*!
 * \brief Gets the next date when evaporation rate changes.
 * \return The current value of NextEvapDate
 */
DateTime climate_getNextEvapDate(void);
/*!
 * \}
 */

/*!
 * \addtogroup Rainfall_Methods Global Rainfall Processing Methods
 * \brief Global rainfall processing methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Opens binary rainfall interface file and RDII processor.
 */
void rain_open(void);

/*!
 * \brief Closes rain interface file and RDII processor.
 */
void rain_close(void);
/*!
 * \}
 */

/*!
 * \addtogroup Snowmelt_Methods Global Snowmelt Processing Methods
 * \brief Global snowmelt processing methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Reads snowmelt parameters from a tokenized line of input data.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details
 * Format of data are:
 * - Name  SubArea   Cmin  Cmax  Tbase  FWF  SD0  FW0  SNN0/SD100
 * - Name  REMOVAL   SDplow Fout Fimperv Fperv Fimelt Fsubcatch (Subcatch)
 */
int snow_readMeltParams(char *tok[], int ntoks);

/*!
 * \brief Creates a snowpack object for a subcatchment.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] snowIndex Snowmelt parameter set index
 * \return TRUE if successful, FALSE if not
 */
int snow_createSnowpack(int subcatchIndex, int snowIndex);

/*!
 * \brief Checks for valid values in a snowmelt parameter set.
 * \param[in] snowIndex Snowmelt parameter set index
 */
void snow_validateSnowmelt(int snowIndex);

/*!
 * \brief Initializes state of a subcatchment's snowpack.
 * \param[in] subcatchIndex Subcatchment index
 */
void snow_initSnowpack(int subcatchIndex);

/*!
 * \brief Initializes values in a snowmelt parameter set.
 * \param[in] snowIndex Snowmelt parameter set index
 */
void snow_initSnowmelt(int snowIndex);

/*!
 * \brief Retrieves the current state of a snowpack object.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] subAreaIndex Snowpack sub-area index
 * \param[out] x Array of snowpack state variables
 */
void snow_getState(int subcatchIndex, int subAreaIndex, double x[]);

/*!
 * \brief Sets the current state of a snowpack object.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] subAreaIndex Snowpack sub-area index
 * \param[in] x Array of snowpack state variables
 */
void snow_setState(int subcatchIndex, int subAreaIndex, double x[]);

/*!
 * \brief Sets values of snowmelt coefficients for a particular time of year.
 * \param[in] snowIndex Snowmelt parameter set index
 * \param[in] season Season of the year
 */
void snow_setMeltCoeffs(int snowIndex, double season);

/*!
 * \brief Adds new snow to subcatchment and plows it between sub-areas.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] tStep Time step (sec)
 */
void snow_plowSnow(int subcatchIndex, double tStep);

/*!
 * \brief Modifies rainfall input to subcatchment's subareas based on possible
 *        snow melt and updates snow depth over entire subcatchment.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] rainfall Rainfall (ft/sec)
 * \param[in] snowfall Snowfall (ft/sec)
 * \param[in] tStep Time step (sec)
 * \param[out] netPrecip Rainfall + snowmelt on each runoff sub-area (ft/sec)
 * \return New snow depth over subcatchment
 */
double snow_getSnowMelt(int subcatchIndex, double rainfall, double snowfall,
                        double tStep, double netPrecip[]);

/*!
 * \brief Computes volume of snow on a subcatchment.
 * \param[in] subcatchIndex Subcatchment index
 * \return Volume of snow cover (ft3)
 */
double snow_getSnowCover(int subcatchIndex);
/*!
 * \}
 */

/*!
 * \addtogroup Runoff_Analyzer_Methods Global Runoff Analyzer Methods
 * \brief Global runoff analyzer methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
 * \brief Opens the runoff analyzer.
 * \return Error code
 */
int runoff_open(void);

/*!
 * \brief Computes runoff for each subcatchment at the current runoff time.
 */
void runoff_execute(void);

/*!
 * \brief Closes the runoff analyzer.
 */
void runoff_close(void);
/*!
 * \}
 */

/*!
 * \addtogroup Runoff_Analyzer_Methods Global Conveyance System Routing Methods
 * \brief Global conveyance system routing methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Initializes the routing analyzer.
 * \return Error code
 */
int routing_open(void);

/*!
 * \brief Determines time step used for flow routing at current time period.
 * \param[in] routingModel Routing method code
 * \param[in] fixedStep User-supplied time step (sec)
 * \return Time step used for flow routing (sec)
 */
double routing_getRoutingStep(int routingModel, double fixedStep);

/*!
 * \brief Executes the routing process at the current time period.
 * \param[in] routingModel Routing method code
 * \param[in] routingStep Routing time step (sec)
 */
void routing_execute(int routingModel, double routingStep);

/*!
 * \brief Closes down the routing analyzer.
 * \param[in] routingModel Routing method code
 */
void routing_close(int routingModel);
/*!
 * \}
 */

/*!
 * \addtogroup Output_File_Methods Global Output File Methods
 * \brief Global output file methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Writes basic project data to binary output file.
 */
int output_open(void);

/*!
 * \brief Writes closing records to binary file.
 */
void output_end(void);

/*!
 * \brief Frees memory used for accessing the binary file.
 */
void output_close(void);

/*!
 * \brief Writes computed results for current report time to binary file.
 * \param[in] reportTime Elapsed simulation time (millisec)
 */
void output_saveResults(double reportTime);

/*!
 * \brief Updates average results for current time period.
 */
void output_updateAvgResults(void);

/*!
 * \brief Retrieves the date/time for a specific reporting period
 * from the binary output file.
 * \param[in] period Index of reporting time period
 * \param[out] days Date/time value
 */
void output_readDateTime(long period, DateTime *days);

/*!
 * \brief Reads computed results for a subcatchment at a specific time
 * period.
 * \param[in] period Index of reporting time period
 * \param[in] index Subcatchment index in binary output file
 */
void output_readSubcatchResults(long period, int index);

/*!
 * \brief Reads computed results for a node at a specific time period.
 * \param[in] period Index of reporting time period
 * \param[in] index Node index in binary output file
 */
void output_readNodeResults(long period, int index);

/*!
 * \brief Reads computed results for a link at a specific time period.
 * \param[in] period Index of reporting time period
 * \param[in] index Link index in binary output file
 */
void output_readLinkResults(long period, int index);
/*!
 * \}
 */

/*!
 * \addtogroup Groundwater_File_Methods Global Groundwater Methods
 * \brief Global groundwater methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Reads aquifer parameter values from line of input data
 * \param[in] aquiferIndex Index of aquifer
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details Data line contains following parameters:
 *  ID, porosity, wiltingPoint, fieldCapacity,     conductivity,
 *  conductSlope, tensionSlope, upperEvapFraction, lowerEvapDepth,
 *  gwRecession,  bottomElev,   waterTableElev,    upperMoisture
 *  (evapPattern)
 */
int gwater_readAquiferParams(int aquiferIndex, char *tok[], int ntoks);

/*!
 * \brief Reads groundwater inflow parameters for a subcatchment from
 * a line of input data.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details Data format is:
 *  subcatch  aquifer  node  surfElev  a1  b1  a2  b2  a3  fixedDepth +
 *            (nodeElev  bottomElev  waterTableElev  upperMoisture )
 */
int gwater_readGroundwaterParams(char *tok[], int ntoks);

/*!
 * \brief Reads mathematical expression for lateral or deep groundwater
 * flow for a subcatchment from a line of input data.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 * \details Format is: subcatch LATERAL/DEEP <expr>
 *  where subcatch is the ID of the subcatchment, LATERAL is for lateral
 *  GW flow, DEEP is for deep GW flow and <expr> is any well-formed math
 *  expression.
 */
int gwater_readFlowExpression(char *tok[], int ntoks);

/*!
 * \brief Veletes a subcatchment's custom groundwater flow expressions.
 * \param[in] subcatchIndex Subcatchment index
 */
void gwater_deleteFlowExpression(int subcatchIndex);

/*!
 * \brief Validates groundwater aquifer properties.
 * \param[in] aquiferIndex Index of aquifer
 */
void gwater_validateAquifer(int aquiferIndex);

/*!
 * \brief Validates groundwater parameters for a subcatchment.
 * \param[in] subcatchIndex Subcatchment index
 */
void gwater_validate(int subcatchIndex);

/*!
 * \brief Initializes state of subcatchment's groundwater.
 * \param[in] subcatchIndex Subcatchment index
 */
void gwater_initState(int subcatchIndex);

/*!
 * \brief Retrieves state of subcatchment's groundwater.
 * \param[in] subcatchIndex Subcatchment index
 * \param[out] x Array of groundwater state variables
 */
void gwater_getState(int subcatchIndex, double x[]);

/*!
 * \brief Assigns values to a subcatchment's groundwater state.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] x Array of groundwater state variables
 */
void gwater_setState(int subcatchIndex, double x[]);

/*!
 * \brief Computes groundwater flow for a subcatchment.
 * \param[in] subcatchIndex Subcatchment index
 * \param[in] evap Pervious surface evaporation volume consumed (ft3)
 * \param[in] infil Surface infiltration volume (ft3)
 * \param[in] tStep Time step (sec)
 */
void gwater_getGroundwater(int subcatchIndex, double evap, double infil,
                           double tStep);

/*!
 * \brief Finds volume of groundwater stored in upper & lower zones.
 * \param[in] subcatchIndex Subcatchment index
 * \return Total volume of groundwater in ft/ft2
 */
double gwater_getVolume(int subcatchIndex);
/*!
 * \}
 */

/*!
 * \addtogroup RDII_File_Methods Global RDII Methods
 * \brief Global RDII methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
 * \brief Reads properties of an RDII inflow from a line of input.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 */
int rdii_readRdiiInflow(char *tok[], int ntoks);

/*!
 * \brief Deletes RDII inflow data for a node.
 * \param[in] nodeIndex Node index
 */
void rdii_deleteRdiiInflow(int nodeIndex);

/*!
 * \brief Initializes properties of a unit hydrograph group.
 * \param[in] unitHydIndex Unit hydrograph group index
 */
void rdii_initUnitHyd(int unitHydIndex);

/*!
 * \brief Reads parameters of an RDII unit hydrograph from a line of input.
 * \param[in] tok Array of string tokens
 * \param[in] ntoks Number of tokens
 * \return Error code
 */
int rdii_readUnitHydParams(char *tok[], int ntoks);

/*!
 * \brief Opens an exisiting RDII interface file or creates a new one.
 */
void rdii_openRdii(void);

/*!
 * \brief Closes the RDII interface file.
 */
void rdii_closeRdii(void);

/*!
 * \brief Finds number of RDII inflows at a specified date.
 * \param[in] aDate Current date/time
 * \return Returns 0 if no RDII flow or number of nodes with RDII inflows
 */
int rdii_getNumRdiiFlows(DateTime aDate);

/*!
 * \brief Finds index and current RDII inflow for an RDII node.
 * \param[in] index RDII node index
 * \param[out] nodeIndex Node index
 * \param[out] q RDII flow rate
 */
void rdii_getRdiiFlow(int index, int *nodeIndex, double *q);
/*!
 * \}
 */

/*!
 * \addtogroup LandUse_File_Methods Global Land Use Methods
 * \brief Global RDII methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
* \brief Reads landuse parameters from a tokenized line of input.
* \param[in] landuseIndex Land use index
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data format is:
*  landuseID  (sweepInterval sweepRemoval sweepDays0)
*/
int landuse_readParams(int landuseIndex, char *tok[], int ntoks);

/*!
* \brief Reads pollutant parameters from a tokenized line of input.
* \param[in] pollutIndex Pollutant index
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data format is:
*  ID Units cRain cGW cRDII kDecay (snowOnly coPollut coFrac cDWF cInit)
*/
int landuse_readPollutParams(int pollutIndex, char *tok[], int ntoks);

/*!
* \brief Reads pollutant buildup parameters from a tokenized line of input.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data format is:
*  landuseID  pollutID  buildupType  c1  c2  c3  normalizerType
*/
int landuse_readBuildupParams(char *tok[], int ntoks);

/*!
* \brief Reads pollutant washoff parameters from a tokenized line of input.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data format is:
*  landuseID  pollutID  washoffType  c1  c2  sweepEffic  bmpRemoval
*/
int landuse_readWashoffParams(char *tok[], int ntoks);

/*!
* \brief Determines the initial buildup of each pollutant on
* each land use for a given subcatchment.
* \param[in] landFactor Land use factor
* \param[out] initBuildup Initial buildup of each pollutant
* \param[in] area Area of subcatchment (ft2)
* \param[in] curb Subcatchment's curb length (users units)
* \details Contributions from co-pollutants to initial buildup are not
* included since the co-pollutant mechanism only applies to washoff.
*/
void landuse_getInitBuildup(TLandFactor *landFactor, double *initBuildup,
                            double area, double curb);

/*!
* \brief Computes new pollutant buildup on a landuse after a time increment.
* \param[in] landuseIndex Land use index
* \param[in] pollutIndex Pollutant index
* \param[in] area Area of subcatchment (ac or ha)
* \param[in] curb Land use curb length (users units)
* \param[in] buildup Current pollutant buildup (lbs or kg)
* \param[in] tStep Time increment for buildup (sec)
* \return Returns new pollutant buildup (lbs or kg)
*/
double landuse_getBuildup(int landuseIndex, int pollutIndex, double area, double curb,
                          double buildup, double tStep);

/*!
* \brief Computes pollutant load generated by a land use over a time step.
* \param[in] landuseIndex Land use index
* \param[in] pollutIndex Pollutant index
* \param[in] area Area of subcatchment (ft2)
* \param[in] landFactor Array of land use data for subcatchment
* \param[in] runoff Runoff flow generated by subcatchment (ft/sec)
* \param[in] vOutflow Runoff volume leaving subcatchment (ft3)
* \return Returns pollutant load generated by a land use over a time step.
*/
double landuse_getWashoffLoad(int landuse, int p, double area,
                              TLandFactor landFactor[], double runoff, double vOutflow);

/*!
* \brief Finds the overall average BMP removal achieved for pollutant pollutIndex
* treated in subcatchment j.
* \param[in] subcatchIndex Subcatchment index
* \param[in] pollutIndex Pollutant index
* \return Returns a BMP removal fraction for a pollutant pollutIndex
*/
double landuse_getAvgBmpEffic(int subcatchIndex, int pollutIndex);

/*!
* \brief Finds washoff mass added by a co-pollutant of a given pollutant.
* \param[in] p Pollutant index
* \param[in] washoff Array of washoff mass for each pollutant
* \return Returns washoff mass added by co-pollutant relation (mass)
*/
double landuse_getCoPollutLoad(int p, double washoff[]);
/*!
 * \}
 */

/*!
 * \addtogroup FlowQuality_Routing_File_Methods Global Flow/Quality Routing Methods
 * \brief Global flow/quality routing methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
* \brief Initializes flow routing system.
* \param[in] routingModel Routing model code
*/
void flowrout_init(int routingModel);

/*!
* \brief Closes down flow routing system.
* \param[in] routingModel Routing model code
*/
void flowrout_close(int routingModel);

/*!
* \brief Finds variable time step for dynamic wave routing.
* \param[in] routingModel Type of routing model
* \param[in] fixedStep User-supplied time step (sec)
* \return Returns adjusted value of routing time step (sec)
*/
double flowrout_getRoutingStep(int routingModel, double fixedStep);

/*!
* \brief Routes flow through conveyance network over current time step.
* \param[in] links Array of link indices
* \param[in] routingModel Routing model code
* \param[in] tStep Time step (sec)
* \return Returns number of computational steps taken
*/
int flowrout_execute(int links[], int routingModel, double tStep);

/*!
* \brief Sorts links from upstream to downstream.
* \param[in, out] sortedLinks Array of link indexes in sorted order
*/
void toposort_sortLinks(int sortedLinks[]);

/*!
* \brief Finds outflow over time step tStep given flow entering a
* conduit using Kinematic Wave flow routing.
* \param[in] linkIndex Link index
* \param[in] qinflow Inflow at current time (cfs)
* \param[out] qoutflow Outflow at current time (cfs)
* \param[in] tStep Time step (sec)
* \return Returns number of iterations used
* \details
*  Orientation of link and variables:
*  q1, a1, q2, a2, q3, a3, x, t 
*                               ^ q3 
*  t                            |   
*  |          qin, ain |-------------------| qout, aout
*  |                   |  Flow --->        |
*  |----> x     q1, a1 |-------------------| q2, a2
*/
int kinwave_execute(int linkIndex, double *qin, double *qout, double tStep);

/*!
* \brief Adjusts dynamic wave routing options.
*/
void dynwave_validate(void);

/*!
* \brief Initializes dynamic wave routing method.
*/
void dynwave_init(void);

/*!
* \brief Frees memory allocated for dynamic wave routing method.
*/
void dynwave_close(void);

/*!
* \brief Computes variable routing time step if applicable.
* \param[in] fixedStep User-supplied fixed time step (sec)
* \return Returns routing time step (sec)
*/
double dynwave_getRoutingStep(double fixedStep);

/*!
* \brief Routes flows through drainage network over current time step.
* \param[in] tStep Time step (sec)
*/
int dynwave_execute(double tStep);

/*!
* \brief Updates flow in conduit link by solving finite difference
* form of continuity and momentum equations.
* \param[in] linkIndex Link index
* \param[in] steps Number of iteration steps taken
* \param[in] omega Under-relaxation parameter
* \param[in] dt Time step (sec)
*/
void dwflow_findConduitFlow(int linkIndex, int steps, double omega, double dt);

/*!
* \brief Initializes water quality concentrations in all nodes and links.
*/
void qualrout_init(void);

/*!
* \brief Routes water quality constituents through the drainage
* network over the current time step.
* \param[in] tStep Routing time step (sec)
*/
void qualrout_execute(double tStep);
/*!
 * \}
 */

/*!
 * \addtogroup Treatment_File_Methods Global Treatment Methods
 * \brief Global treatment methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
* \brief Allocates memory for computing pollutant removals by treatment.
* \return Returns True if successful, False if not
*/
int treatmnt_open(void);

/*!
* \brief Frees memory used for computing pollutant removals by treatment.
*/
void treatmnt_close(void);

/*!
* \brief Reads a treatment expression from a tokenized line of input.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
*/
int treatmnt_readExpression(char *tok[], int ntoks);

/*!
* \brief Deletes the treatment objects for each pollutant at a node.
* \param[in] nodeIndex Node index
*/
void treatmnt_delete(int nodeIndex);

/*!
* \brief Updates pollutant concentrations at a node after treatment.
* \param[in] nodeIndex Node index
* \param[in] q Inflow to node (cfs)
* \param[in] v Volume of node (ft3)
* \param[in] tStep Routing time step (sec)
*/
void treatmnt_treat(int nodeIndex, double q, double v, double tStep);

/*!
* \brief Computes and saves array of inflow concentrations to a node.
* \param[in] qIn Flow inflow rate (cfs)
* \param[in] wIn Pollutant mass inflow rates (mass/sec)
*/
void treatmnt_setInflow(double qIn, double wIn[]);
/*!
 * \}
 */

/*!
 * \addtogroup Mass_Balance_File_Methods Global Mass Balance Methods
 * \brief Global mass balance methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
* \brief Opens and initializes mass balance continuity checking.
* \return Error code
*/
int massbal_open(void);

/*!
* \brief Frees memory used by mass balance system.
*/
void massbal_close(void);

/*!
* \brief Reports mass balance results.
*/
void massbal_report(void);

/*!
* \brief Updates runoff totals after current time step.
* \param[in] flowType Type of flow (RUNOFF_RAINFALL, RUNOFF_EVAP, RUNOFF_RUNOFF, etc.)
* \param[in] v Volume of flow (ft3)
*/
void massbal_updateRunoffTotals(int flowType, double v);

/*!
* \brief Adds inflow mass loading to loading totals for current time step.
* \param[in] type Type of inflow
* \param[in] pollutIndex Pollutant index
* \param[in] w Mass loading
*/
void massbal_updateLoadingTotals(int type, int pollutIndex, double w);

/*!
* \brief Updates groundwater totals after current time step.
* \param[in] vInfil Volume depth of infiltration (ft)
* \param[in] vUpperEvap Volume depth of upper zone evaporation (ft)
* \param[in] vLowerEvap Volume depth of lower zone evaporation (ft)
* \param[in] vLowerPerc Volume depth of percolation to deep GW (ft)
* \param[in] vGwater Volume depth of groundwater outflow (ft)
*/
void massbal_updateGwaterTotals(double vInfil, double vUpperEvap,
                                double vLowerEvap, double vLowerPerc, double vGwater);

/*!
* \brief Updates overall routing totals with totals from current time step.
* \param[in] tStep Time step (sec)
*/
void massbal_updateRoutingTotals(double tStep);

/*!
* \brief Initializes routing totals for current time step.
*/
void massbal_initTimeStepTotals(void);

/*!
* \brief Adds flow inflow to routing totals for current time step.
* \param[in] type Type of inflow
* \param[in] q Inflow rate (cfs)
*/
void massbal_addInflowFlow(int type, double q);

/*!
* \brief Adds quality inflow to routing totals for current time step.
* \param[in] type Type of inflow
* \param[in] pollutIndex Pollutant index
* \param[in] w Mass flow rate (mass/sec)
*/
void massbal_addInflowQual(int type, int pollutIndex, double w);

/*!
* \brief Adds flow outflow over current time step to routing totals.
* \param[in] q Outflow rate (cfs)
* \param[in] isFlooded TRUE if outflow represents internal flooding
*/
void massbal_addOutflowFlow(double q, int isFlooded);

/*!
* \brief Adds pollutant outflow over current time step to routing totals.
* \param[in] pollutIndex Pollutant index
* \param[in] mass Mass outflow rate (mass/sec)
* \param[in] isFlooded TRUE if outflow represents internal flooding
*/
void massbal_addOutflowQual(int pollutIndex, double mass, int isFlooded);

/*!
* \brief Adds node losses over current time step to routing totals.
* \param[in] evapLoss Evaporation loss from all nodes (ft3/sec)
* \param[in] infilLoss Seepage loss from all nodes (ft3/sec)
*/
void massbal_addNodeLosses(double evapLoss, double infilLoss);

/*!
* \brief Adds link losses over current time step to routing totals.
* \param[in] evapLoss Evaporation loss from all links (ft3/sec)
* \param[in] infilLoss Infiltration loss from all links (ft3/sec)
*/
void massbal_addLinkLosses(double evapLoss, double infilLoss);

/*!
* \brief Adds mass reacted during current time step to routing totals.
* \param[in] pollutIndex Pollutant index
* \param[in] mass Rate of mass reacted (mass/sec)
*/
void massbal_addReactedMass(int pollutIndex, double mass);

/*!
* \brief Adds mass lost to seepage during current time step to routing totals.
* \param[in] pollutIndex Pollutant index
* \param[in] seepLoss Mass seepage rate (mass/sec)
*/
void massbal_addSeepageLoss(int pollutIndex, double seepLoss);

/*!
* \brief Adds mass remaining on dry surface to routing totals.
* \param[in] pollutIndex Pollutant index
* \param[in] mass Pollutant mass
*/
void massbal_addToFinalStorage(int pollutIndex, double mass);

/*!
* \brief Computes flow routing mass balance error at current time step.
* \return Returns fractional difference between total inflow and outflow.
*/
double massbal_getStepFlowError(void);

/*!
* \brief Computes runoff mass balance error.
* \return Returns percent runoff mass balance error.
*/
double massbal_getRunoffError(void);

/*!
* \brief Computes flow routing mass balance error.
* \return Returns percent flow routing mass balance error.
*/
double massbal_getFlowError(void);
/*!
 * \}
 */

/*!
 * \addtogroup Simulation_Statistics_File_Methods Global Simulation Statistics Methods
 * \brief Global simulation statistics methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
* \brief Opens the simulation statistics system.
* \return Error code
*/
int stats_open(void);

/*!
* \brief Closes the simulation statistics system.
*/
void stats_close(void);

/*!
* \brief Reports simulation statistics.
*/
void stats_report(void);

/*!
* \brief Updates count of times a node or link was time step-critical.
* \param[in] nodeIndex Node index
* \param[in] linkIndex Link index
*/
void stats_updateCriticalTimeCount(int nodeIndex, int linkIndex);

/*!
* \brief Updates various flow routing statistics at current time period.
* \param[in] tStep Routing time step (sec)
* \param[in] aDate Current date/time
*/
void stats_updateFlowStats(double tStep, DateTime aDate);

/*!
* \brief Updates flow routing time step statistics.
* \param[in] tStep Current flow routing time step (sec)
* \param[in] trialsCount Number of trials used to solve routing
* \param[in] steadyState TRUE if steady flow conditions exist
*/
void stats_updateTimeStepStats(double tStep, int trialsCount, int steadyState);

/*!
* \brief Updates totals of runoff components for a specific subcatchment.
* \param[in] subcatchIndex Subcatchment index
* \param[in] rainVol Rainfall + snowfall volume (ft3)
* \param[in] runonVol Runon volume from other subcatchments (ft3)
* \param[in] evapVol Evaporation volume (ft3)
* \param[in] infilVol Infiltration volume (ft3)
* \param[in] impervVol Impervious runoff volume (ft3)
* \param[in] pervVol Pervious runoff volume (ft3)
* \param[in] runoffVol Total runoff volume (ft3)
* \param[in] runoff Runoff rate (cfs)
*/
void stats_updateSubcatchStats(int subcatchIndex, double rainVol,
                               double runonVol, double evapVol, double infilVol,
                               double impervVol, double pervVol, double runoffVol, double runoff);

/*!
* \brief Updates groundwater statistics for a specific subcatchment.
* \param[in] subcatchIndex Subcatchment index
* \param[in] infil Infiltration volume (ft3)
* \param[in] evap Evaporation volume (ft3)
* \param[in] latFlow Lateral flow volume (ft3)
* \param[in] deepFlow Deep flow volume (ft3)
* \param[in] theta Soil moisture content
* \param[in] waterTable Depth to water table (ft)
* \param[in] tStep Time step (sec)
*/
void stats_updateGwaterStats(int subcatchIndex, double infil, double evap,
                             double latFlow, double deepFlow, double theta, double waterTable,
                             double tStep);

/*!
* \brief Updates value of maximum system runoff rate.
* \note Updates global variable MaxRunoffFlow
*/
void stats_updateMaxRunoff(void);

/*!
* \brief Updates a node's maximum depth recorded at reporting times.
* \param[in] nodeIndex Node index
* \param[in] depth Water depth at node at current reporting time (ft)
*/
void stats_updateMaxNodeDepth(int nodeIndex, double depth);

/*!
* \brief Updates convergence statistics node
* \param[in] nodeIndex Node index
* \param[in] converged TRUE if node has converged
*/
void stats_updateConvergenceStats(int nodeIndex, int converged);
/*!
 * \}
 */

/*!
 * \addtogroup Raingage_File_Methods Global Raingage Methods
 * \brief Global raingage methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
* \brief Reads rain gage parameters from a line of input data
* \param[in] gageIndex Gage index
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data formats are:
*  Name RainType RecdFreq SCF TIMESERIES SeriesName
*  Name RainType RecdFreq SCF FILE FileName Station Units StartDate
*/
int gage_readParams(int gageIndex, char *tok[], int ntoks);

/*!
* \brief Checks for valid rain gage parameters
* \param[in] gageIndex Gage index
* \note Assumes that any time series used by the gage has been previously validated
*/
void gage_validate(int gageIndex);

/*!
* \brief Initializes state of a rain gage
* \param[in] gageIndex Rain gage index
*/
void gage_initState(int gageIndex);

/*!
* \brief Updates state of rain gage for specified date. 
* \param[in] gageIndex Rain gage index
* \param[in] aDate A calendar date/time
*/
void gage_setState(int gageIndex, DateTime aDate);

/*!
* \brief Determines whether gage's recorded rainfall is rain or snow.
* \param[in] gageIndex Rain gage index
* \param[out] rainfall Rainfall rate (ft/sec)
* \param[out] snowfall Snowfall rate (ft/sec)
* \return Returns Total precipitation (ft/sec)
*/
double gage_getPrecip(int gageIndex, double *rainfall, double *snowfall);

/*!
* \brief Sets the rainfall value reported at the current reporting time.
* \param[in] gageIndex Rain gage index
* \param[in] aDate Current date/time
*/
void gage_setReportRainfall(int gageIndex, DateTime aDate);

/*!
* \brief Finds the next date from  specified date when rainfall occurs.
* \param[in] gageIndex Rain gage index
* \param[in] aDate Current date/time
* \return Returns next date when rainfall occurs
*/
DateTime gage_getNextRainDate(int gageIndex, DateTime aDate);

/*!
* \brief Updates past MAXPASTRAIN hourly rain totals.
* \param[in] gageIndex Rain gage index
* \param[in] tStep Time step (sec)
* \details
* pastRain[0] is past rain volume prior to 1 hour,
* pastRain[n] is past rain volume after n hours,
* pastInterval is time since last hour was reached.
*/
void gage_updatePastRain(int gageIndex, int tStep);

/*!
* \brief Retrieves rainfall total over some previous number of hours.
* \param[in] gageIndex Rain gage index
* \param[in] hrs Number of hours
* \return Returns cumulative rain volume (inches or mm) in last n hours
*/
double gage_getPastRain(int gageIndex, int hrs);
/*!
 * \}
 */

/*!
 * \addtogroup Subcatchment_File_Methods Global Subcatchments Methods
 * \brief Global subcatchment methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
* \brief Reads subcatchment parameters from a tokenized  line of input data.
* \param[in] subcatchIndex Subcatchment index
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data has format:
*  Name  RainGage  Outlet  Area  %Imperv  Width  Slope CurbLength  Snowpack
*/
int subcatch_readParams(int subcatchIndex, char *tok[], int ntoks);

/*!
* \brief Rreads subcatchment's subarea parameters from a tokenized 
* line of input data.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data has format:
*  Subcatch  Imperv_N  Perv_N  Imperv_S  Perv_S  PctZero  RouteTo (PctRouted)
*/
int subcatch_readSubareaParams(char *tok[], int ntoks);

/*!
* \brief Reads assignment of landuses to subcatchment from a tokenized 
* line of input data.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data has format:
*  Subcatch  landuse  percent .... landuse  percent
*/
int subcatch_readLanduseParams(char *tok[], int ntoks);

/*!
* \brief Reads initial pollutant buildup on subcatchment from 
* tokenized line of input data.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data has format:
*  Subcatch  pollut  initLoad .... pollut  initLoad
*/
int subcatch_readInitBuildup(char *tok[], int ntoks);

/*!
* \brief Checks for valid subcatchment input parameters.
* \param[in] subcatchIndex Subcatchment index
*/
void subcatch_validate(int subcatchIndex);

/*!
* \brief Initializes state of a subcatchment.
* \param[in] subcatchIndex Subcatchment index
*/
void subcatch_initState(int subcatchIndex);

/*!
* \brief Replaces old state of subcatchment with new state.
* \param[in] subcatchIndex Subcatchment index
*/
void subcatch_setOldState(int subcatchIndex);

/*!
* \brief Determines what fraction of subcatchment area, including any LID
* area, is pervious.
* \param[in] subcatchIndex Subcatchment index
* \return Returns fraction of subcatchment area that is pervious cover
*/
double subcatch_getFracPerv(int subcatchIndex);

/*!
* \brief Finds total volume of water stored on a subcatchment's surface
* and its LIDs at the current time.
* \param[in] subcatchIndex Subcatchment index
* \return Returns total volume of water stored on subcatchment's surface (ft3)
*/
double subcatch_getStorage(int subcatchIndex);

/*!
* \brief Finds average depth of water over the non-LID portion of a
* subcatchment
* \param[in] subcatchIndex Subcatchment index
* \return Returns average depth of ponded water (ft)
*/
double subcatch_getDepth(int subcatchIndex);

/*!
* \brief Routes runoff from a subcatchment to its outlet subcatchment
* or between its subareas.
* \param[in] subcatchIndex Subcatchment index
*/
void subcatch_getRunon(int subcatchIndex);

/*!
* \brief Updates the total runon flow (ft/s) seen by a subcatchment that
* receives runon flow from an upstream subcatchment.
* \param[in] subcatchIndex Subcatchment index
* \param[in] flow Runon flow rate (cfs) to subcatchment subcatchIndex
*/
void subcatch_addRunonFlow(int subcatchIndex, double flow);

/*!
* \brief Computes runoff & new storage depth for subcatchment.
* \param[in] subcatchIndex Subcatchment index
* \param[in] tStep Time step (sec)
* \details
*  The 'runoff' value returned by this function is the total runoff
*  generated (in ft/sec) by the subcatchment before any internal
*  re-routing is applied. It is used to compute pollutant washoff.
*  
*  The 'outflow' value computed here (in cfs) is the surface runoff
*  that actually leaves the subcatchment after any LID controls are
*  applied and is saved to Subcatch[j].newRunoff. 
*/
double subcatch_getRunoff(int subcatchIndex, double tStep);

/*!
* \brief Computes weighted combination of old and new subcatchment runoff.
* \param[in] subcatchIndex Subcatchment index
* \param[in] wt Weighting factor
* \return Returns weighted runoff value (ft/sec)
*/
double subcatch_getWtdOutflow(int subcatchIndex, double wt);

/*!
* \brief Computes wtd. combination of old and new subcatchment results.
* \param[in] subcatchIndex Subcatchment index
* \param[in] wt Weighting factor
* \param[out] x Array of subcatchment results
*/
void subcatch_getResults(int subcatchIndex, double wt, float x[]);
/*!
 * \}
 */

/*!
 * \addtogroup Surface_Pollutant_File_Methods Global Surface Pollutant Buildup/Washoff Methods
 * \brief Global surface pollutant buildup/washoff methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
* \brief Initializes pollutant buildup, ponded mass, and washoff.
* \param[in] subcatchIndex Subcatchment index
*/
void surfqual_initState(int subcatchIndex);

/*!
* \brief Computes new runoff quality for a subcatchment.
* \param[in] subcatchIndex Subcatchment index
* \param[in] runoff Total subcatchment runoff before internal re-routing or
* LID controls (ft/sec)
* \param[in] tStep Time step (sec)
* \details
* Considers three pollutant generating streams that are combined together:
* 1. washoff of pollutant buildup as described by the project's land use
*    washoff functions,
* 2. complete mix mass balance of pollutants in surface ponding on
*    non-LID area due to runon, wet deposition, infiltration, & evaporation,
* 3. wet deposition and runon over LID areas.
*/
void surfqual_getWashoff(int subcatchIndex, double runoff, double tStep);

/*!
* \brief Adds to pollutant buildup on subcatchment surface.
* \param[in] subcatchIndex Subcatchment index
* \param[in] tStep Time step (sec)
*/
void surfqual_getBuildup(int subcatchIndex, double tStep);

/*!
* \brief Reduces pollutant buildup over a subcatchment if sweeping occurs.
* \param[in] subcatchIndex Subcatchment index
* \param[in] aDate Current date/time
*/
void surfqual_sweepBuildup(int subcatchIndex, DateTime aDate);

/*!
* \brief Finds wtd. combination of old and new washoff for a pollutant.
* \param[in] subcatchIndex Subcatchment index
* \param[in] pollutIndex Pollutant index
* \param[in] wt Weighting factor
* \return Returns pollutant washoff value
*/
double surfqual_getWtdWashoff(int subcatchIndex, int pollutIndex, double wt);
/*!
 * \}
 */

/*!
 * \addtogroup Conveyance_System_Node_File_Methods Global Conveyance System Node Methods
 * \brief Global conveyance system node methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */

/*!
* \brief Reads node properties from a tokenized line of input.
* \param[in] nodeIndex Node index
* \param[in] type Node type code
* \param[in] subIndex Node sub-type code
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
*/
int node_readParams(int nodeIndex, int type, int subIndex, char *tok[], int ntoks);

/*!
* \brief Validates a node's properties.
* \param[in] nodeIndex Node index
*/
void node_validate(int nodeIndex);

/*!
* \brief Initializes a node's state variables at start of simulation.
* \param[in] nodeIndex Node index
*/
void node_initState(int nodeIndex);

/*!
* \brief Initializes a node's inflow/outflow/overflow at start of time step.
* \param[in] nodeIndex Node index
* \param[in] tStep Time step (sec)
*/
void node_initFlows(int nodeIndex, double tStep);

/*!
* \brief Replaces a node's old hydraulic state values with new ones.
* \param[in] nodeIndex Node index
*/
void node_setOldHydState(int nodeIndex);

/*!
* \brief Replaces a node's old water quality state values with new ones.
* \param[in] nodeIndex Node index
*/
void node_setOldQualState(int nodeIndex);

/*!
* \brief Sets water depth at a node that serves as an outlet point.
* \param[in] nodeIndex Node index
* \param[in] yNorm Normal flow depth (ft)
* \param[in] yCrit Critical flow depth (ft)
* \param[in] z Offset of connecting outfall link from node invert (ft)
*/
void node_setOutletDepth(int nodeIndex, double yNorm, double yCrit, double z);

/*!
* \brief Computes surface area of water stored at a node from water depth.
* \param[in] nodeIndex Node index
* \param[in] depth Water depth (ft)
* \return Returns surface area of water stored at node (ft2)
*/
double node_getSurfArea(int nodeIndex, double depth);

/*!
* \brief Computes a node's water depth from its volume.
* \param[in] nodeIndex Node index
* \param[in] volume Volume of water stored at node (ft3)
* \return Returns water depth at node (ft)
*/
double node_getDepth(int nodeIndex, double volume);

/*!
* \brief Computes volume stored at a node from its water depth.
* \param[in] nodeIndex Node index
* \param[in] depth Water depth at node (ft)
* \return Returns volume of water stored at node (ft3)
*/
double node_getVolume(int nodeIndex, double depth);

/*!
* \brief Computes surface area of water at a node based on depth.
* \param[in] nodeIndex Node index
* \param[in] depth Water depth at node (ft)
* \return Returns surface area of water at node (ft2)
*/
double node_getPondedArea(int nodeIndex, double depth);

/*!
* \brief Computes outflow from node available for inflow into a link.
* \param[in] nodeIndex Node index
* \param[in] linkIndex Link index
* \return Returns flow rate leaving node (cfs)
*/
double node_getOutflow(int nodeIndex, int linkIndex);

/*!
* \brief Computes the rates of evaporation and infiltration over a given
* time step for a node.
* \param[in] nodeIndex Node index
* \param[in] tStep Time step (sec)
* \return Returns water loss rate at node (ft3)
*/
double node_getLosses(int nodeIndex, double tStep);

/*!
* \brief Limits outflow rate from a node with storage volume.
* \param[in] nodeIndex Node index
* \param[in] q Flow rate leaving node (cfs)
* \param[in] tStep Time step (sec)
* \return Returns modified flow rate (cfs)
*/
double node_getMaxOutflow(int nodeIndex, double q, double tStep);

/*!
* \brief Computes flow rate at outfalls and flooded nodes.
* \param[in] nodeIndex Node index
* \param[out] isFlooded TRUE if node is flooded
* \return Returns flow rate lost from system (cfs)
*/
double node_getSystemOutflow(int nodeIndex, int *isFlooded);

/*!
* \brief Computes weighted average of old and new results at a node.
* \param[in] nodeIndex Node index
* \param[in] wt Weighting factor
* \param[out] x Array of node results
*/
void node_getResults(int nodeIndex, double wt, float x[]);
/*!
 * \}
 */

/*!
 * \addtogroup Conveyance_System_Inflow_File_Methods Global Conveyance System Inflow Methods
 * \brief Global conveyance system inflow methods
 * \ingroup Global_Interfacing_Functions
 * \{
 */
/*!
* \brief Reads parameters of a direct external inflow from a line of input.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Data formats of data line are:
* nodeID  FLOW      tSeriesID  (FLOW         1.0          scaleFactor  baseline  basePat)
* nodeID  pollutID  tSeriesID  (CONCEN/MASS  unitsFactor  scaleFactor  baseline  basePat)
*/
int inflow_readExtInflow(char *tok[], int ntoks);

/*!
* \brief Reads dry weather inflow parameters from line of input data.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Format of data line is:
* nodeID  FLOW/pollutID  avgValue  (pattern1 pattern2  ... pattern4)
*/
int inflow_readDwfInflow(char *tok[], int ntoks);

/*!
* \brief Reads values of a time pattern from a line of input data.
* \param[in] tok Array of string tokens
* \param[in] ntoks Number of tokens
* \return Error code
* \details Format of data line is:
* patternID  patternType  value(1) value(2) ...
* patternID  value(n)  value(n+1) ....          (for continuation lines)
*/
int inflow_readDwfPattern(char *tok[], int ntoks);

/*!
* \brief This function assigns property values to the inflow object.
* \param[in] nodeIndex Node index
* \param[in] param Parameter code
* \param[in] type Inflow type code
* \param[in] tSeries Time series index
* \param[in] basePat Base pattern index
* \param[in] cf Conversion factor
* \param[in] baseline Baseline value
* \param[in] sf Scale factor
* \return Error code
*/
int inflow_setExtInflow(int nodeIndex, int param, int type, int tSeries,
                        int basePat, double cf, double baseline, double sf);

/*!
* \brief Initialzes a dry weather inflow by ordering its time patterns.
* \param[in] inflow Dry weather inflow object
* \details This function sorts the user-supplied time patterns for a dry weather
* inflow in the order of the PatternType enumeration (monthly, daily,
* weekday hourly, weekend hourly) to help speed up pattern processing.
*/
void inflow_initDwfInflow(TDwfInflow *inflow);

/*!
* \brief Initialzes a dry weather inflow time pattern.
* \param[in] patternIndex Time pattern index
*/
void inflow_initDwfPattern(int patternIndex);

/*!
* \brief Retrieves the value of an external inflow at a specific
* date and time.
* \param[in] inflow External inflow data structure
* \param[in] aDate Current simulation date/time
* \return Returns current value of external inflow parameter
*/
double inflow_getExtInflow(TExtInflow *inflow, DateTime aDate);

/*!
* \brief Computes dry weather inflow value at a specific point in time.
* \param[in] inflow Dry weather inflow data structure
* \param[in] m Current month of year for the simulation
* \param[in] d Current day of month for the simulation
* \param[in] h Current hour of day for the simulation
* \return Returns dry weather inflow value
*/
double inflow_getDwfInflow(TDwfInflow *inflow, int m, int d, int h);

/*!
* \brief Deletes all time series inflow data for a node.
* \param[in] nodeIndex Node index
*/
void inflow_deleteExtInflows(int nodeIndex);

/*!
* \brief Deletes all dry weather inflow data for a node.
* \param[in] nodeIndex Node index
*/
void inflow_deleteDwfInflows(int nodeIndex);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Routing Interface File Methods
//-----------------------------------------------------------------------------
int iface_readFileParams(char *tok[], int ntoks);
void iface_openRoutingFiles(void);
void iface_closeRoutingFiles(void);
int iface_getNumIfaceNodes(DateTime aDate);
int iface_getIfaceNode(int index);
double iface_getIfaceFlow(int index);
double iface_getIfaceQual(int index, int pollut);
void iface_saveOutletResults(DateTime reportDate, FILE *file);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Hot Start File Methods
//-----------------------------------------------------------------------------
int hotstart_open(void);
void hotstart_save(void);
int hotstart_save_to_file(const char *hotstartFile);
int hotstart_is_valid(const char *hotstartFile, int *inputFileVersion);
void hotstart_close(void);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Conveyance System Link Methods
//-----------------------------------------------------------------------------
int link_readParams(int link, int type, int subIndex, char *tok[], int ntoks);
int link_readXsectParams(char *tok[], int ntoks);
int link_readLossParams(char *tok[], int ntoks);

void link_validate(int link);
void link_initState(int link);
void link_setOldHydState(int link);
void link_setOldQualState(int link);

void link_setTargetSetting(int j);
void link_setSetting(int j, double tstep);
int link_setFlapGate(int link, int n1, int n2, double q);

double link_getInflow(int link);
void link_setOutfallDepth(int link);
double link_getLength(int link);
double link_getYcrit(int link, double q);
double link_getYnorm(int link, double q);
double link_getVelocity(int link, double q, double y);
double link_getFroude(int link, double v, double y);
double link_getPower(int link);
double link_getLossRate(int link, int routeModel, double q, double tstep);
char link_getFullState(double a1, double a2, double aFull);

void link_getResults(int link, double wt, float x[]);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Link Cross-Section Methods
//-----------------------------------------------------------------------------
int xsect_isOpen(int type);
int xsect_setParams(TXsect *xsect, int type, double p[], double ucf);
void xsect_setIrregXsectParams(TXsect *xsect);
void xsect_setCustomXsectParams(TXsect *xsect);
void xsect_setStreetXsectParams(TXsect *xsect);
double xsect_getAmax(TXsect *xsect);

double xsect_getSofA(TXsect *xsect, double area);
double xsect_getYofA(TXsect *xsect, double area);
double xsect_getRofA(TXsect *xsect, double area);
double xsect_getAofS(TXsect *xsect, double sFactor);
double xsect_getdSdA(TXsect *xsect, double area);
double xsect_getAofY(TXsect *xsect, double y);
double xsect_getRofY(TXsect *xsect, double y);
double xsect_getWofY(TXsect *xsect, double y);
double xsect_getYcrit(TXsect *xsect, double q);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Culvert/Roadway Methods
//-----------------------------------------------------------------------------
double culvert_getInflow(int link, double q, double h);
double roadway_getInflow(int link, double dir, double hcrest, double h1,
                         double h2);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Force Main Methods
//-----------------------------------------------------------------------------
double forcemain_getEquivN(int j, int k);
double forcemain_getRoughFactor(int j, double lengthFactor);
double forcemain_getFricSlope(int j, double v, double hrad);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Cross-Section Transect Methods
//-----------------------------------------------------------------------------
int transect_create(int n);
void transect_delete(void);
int transect_readParams(int *count, char *tok[], int ntoks);
void transect_validate(int j);
void transect_createStreetTransect(TStreet *street);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Street Cross-Section Methods
//-----------------------------------------------------------------------------
int street_create(int nStreets);
void street_delete();
int street_readParams(char *tok[], int ntoks);
double street_getExtentFilled(int link);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Custom Shape Cross-Section Methods
//-----------------------------------------------------------------------------
int shape_validate(TShape *shape, TTable *curve);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Control Rule Methods
//-----------------------------------------------------------------------------
int controls_create(int n);
void controls_delete(void);
void controls_init(void);
void controls_addToCount(char *s);
int controls_addVariable(char *tok[], int ntoks);
int controls_addExpression(char *tok[], int ntoks);
int controls_addRuleClause(int rule, int keyword, char *Tok[], int nTokens);
int controls_evaluate(DateTime currentTime, DateTime elapsedTime,
                      double tStep);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Table & Time Series Methods
//-----------------------------------------------------------------------------
int table_readCurve(char *tok[], int ntoks);
int table_readTimeseries(char *tok[], int ntoks);

int table_addEntry(TTable *table, double x, double y);
int table_getFirstEntry(TTable *table, double *x, double *y);
int table_getNextEntry(TTable *table, double *x, double *y);
void table_deleteEntries(TTable *table);

void table_init(TTable *table);
int table_validate(TTable *table);

double table_lookup(TTable *table, double x);
double table_lookupEx(TTable *table, double x);
double table_intervalLookup(TTable *table, double x);
double table_inverseLookup(TTable *table, double y);

double table_getSlope(TTable *table, double x);
double table_getMaxY(TTable *table, double x);
double table_getStorageVolume(TTable *table, double x);
double table_getStorageDepth(TTable *table, double v);

void table_tseriesInit(TTable *table);
double table_tseriesLookup(TTable *table, double t, char extend);
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//   Utility Methods
//-----------------------------------------------------------------------------
double UCF(int quantity);                    // units conversion factor
int getInt(char *s, int *y);                 // get integer from string
int getFloat(char *s, float *y);             // get float from string
int getDouble(char *s, double *y);           // get double from string
char *getTempFileName(char *s);              // get temporary file name
int findmatch(char *s, char *keyword[]);     // search for matching keyword
int match(char *str, char *substr);          // true if substr matches part of str
int strcomp(const char *s1, const char *s2); // case insensitive string compare
size_t sstrncpy(char *dest, const char *src,
                size_t n); // safe string copy
size_t sstrcat(char *dest, const char *src,
               size_t destsize);          // safe string concatenation
void writecon(const char *s);             // writes string to console
DateTime getDateTime(double elapsedMsec); // convert elapsed time to date
void getElapsedTime(DateTime aDate,       // convert elapsed date
                    int *days, int *hrs, int *mins);
char *addAbsolutePath(char *fname); // add full path to a file name
/*!
 * \}
 */

/*!
 * \}
 */

#endif // FUNCS_H
