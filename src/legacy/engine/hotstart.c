//-----------------------------------------------------------------------------
//   hotstart.c
//
//   Project:  EPA SWMM5
//   Version:  5.2
//   Date:     11/01/21 (Build 5.2.0)
//   Author:   L. Rossman
//
//   Hot Start file functions.
//
//   A SWMM hot start file contains the state of a SWMM project after
//   a simulation has been run, allowing it to be used to initialize
//   a subsequent simulation that picks up where the previous run ended.
//
//   An abridged version of the hot start file (version 2) is available
//   that contains only variables that appear in the binary output file
//   (groundwater upper moisture and water table elevation, node depth,
//   lateral inflow, and quality, and link flow, depth, setting and quality).
//
//   When reading a previously saved hot start file checks are made to
//   insure the the current SWMM project has the same number of major
//   components (subcatchments, land uses, nodes, links, and pollutants)
//   and unit system as does the hot start file. No test is made to
//   insure that these components are of the same sub-type and maintain
//   the same order as when the hot start file was created.
//
//   Update History
//   ==============
//   Build 5.1.008:
//   - Storage node hydraulic residence time (HRT) was added to the file.
//   - Link control settings are now applied when reading a hot start file.
//   - Runoff read from file assigned to newRunoff property instead of oldRunoff.
//   - Array indexing bug when reading snowpack state from file fixed.
//   Build 5.1.011:
//   - Link control setting bug when reading a hot start file fixed.
//   Build 5.1.015:
//   - Support added for multiple infiltration methods within a project.
//   Build 5.2.5:
//   - Fixed bug in fwrite count argument when writing catchment landuse pollutant
//     build-up.
//   Build 5.3.0
//   - Modified code to allow saving multiple hotstart files at different times.
//-----------------------------------------------------------------------------

/**
 * @file      hotstart.c
 * @brief     Reading and writing of legacy hot-start state files.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "headers.h"

//-----------------------------------------------------------------------------
//  Local Variables
//-----------------------------------------------------------------------------
static int fileVersion;

//-----------------------------------------------------------------------------
//  External functions (declared in funcs.h)
//-----------------------------------------------------------------------------
// hotstart_open                          (called by swmm_start in swmm5.c)
// hotstart_close                         (called by swmm_end in swmm5.c)

//-----------------------------------------------------------------------------
// Function declarations
//-----------------------------------------------------------------------------
static int initializeFromHotstartFile(void);
static int initializeSaveHotstartFile(TFile* hotstartFile);
static void readRunoff(void);
static void saveRunoff(TFile* hotstartFile);
static void readRouting(void);
static void saveRouting(TFile* hotstartFile);
static int readFloat(float* x, FILE* f);
static int readDouble(double* x, FILE* f);

//=============================================================================

/**
 * @brief Restore configured input state and initialize configured hotstart outputs.
 * @return TRUE if input restoration and all output opens succeed, FALSE
 * after a reported input/output initialization failure.
 * @post Successful output streams remain open for the hotstart lifecycle.
 * On a later failure, earlier successful output opens remain for cleanup;
 * this routine does not roll back model state restored from the input.
 */
int hotstart_open() {
    int i;
    int errorCode = 0;

    // --- open hot start files
    if (!initializeFromHotstartFile())
        return FALSE; //input hot start file

    for (i = 0; i < MAXHOTSTARTFILES; i++) {
        if ((errorCode = initializeSaveHotstartFile(&FhotstartOutputs[i]))) {
            report_writeErrorMsg(errorCode, FhotstartOutputs[i].name);
            return FALSE;
        }
    }

    return TRUE;
}

//=============================================================================

/**
 * @brief Saves hotstart files whose specified save time has arrived.
 */
void hotstart_save() {
    int i;

    for (i = 0; i < MAXHOTSTARTFILES; i++) {
        if (FhotstartOutputs[i].file && FhotstartOutputs[i].saveDateTime > 0 &&
            NewRoutingTime >= FhotstartOutputs[i].saveDateTime) {
            saveRunoff(&FhotstartOutputs[i]);
            saveRouting(&FhotstartOutputs[i]);
            fclose(FhotstartOutputs[i].file);
            FhotstartOutputs[i].file = NULL;
        }
    }
}

//=============================================================================

/**
 * @brief Save the current runoff and routing state to a named version-4 hotstart file.
 * @param[in] hotstartFile Non-null borrowed null-terminated filename;
 * relative paths are resolved through addAbsolutePath().
 * @return Zero after opening and writing the snapshot, or the opening error
 * code from initializeSaveHotstartFile(). Individual writes/close are not checked.
 * @note Creates or truncates the destination, closes its local stream, and
 * frees its temporary descriptor. Allocation failure is not checked here.
 */
int hotstart_save_to_file(const char* hotstartFile)
//
//  Input:   hotStartFile = filepath to hotstart file to use
//  Output:  returns error code
//  Purpose: Saves and closes hotstart files that are to be written at end of simulation.
{
    int error_code = 0;
    char fname[MAXFNAME + 1];
    TFile* hotstart = (TFile*)calloc(1, sizeof(TFile));

    hotstart->file = NULL;
    hotstart->mode = SAVE_FILE;
    hotstart->saveDateTime = 0;

    sstrncpy(fname, hotstartFile, MAXFNAME);
    sstrncpy(hotstart->name, addAbsolutePath(fname), MAXFNAME);

    if ((error_code = initializeSaveHotstartFile(hotstart))) {
        FREE(hotstart);
        return error_code;
    } else {
        saveRunoff(hotstart);
        saveRouting(hotstart);
        fclose(hotstart->file);
        hotstart->file = NULL;
        FREE(hotstart);
        return error_code;
    }
}

//=============================================================================

/**
 * @brief Recognize a hotstart file's header and version.
 * @param[in] hotstartFile Non-null borrowed null-terminated path; opened in
 * r+b mode, so writable access is required even though only the header is read.
 * @param[out] inputFileVersion Non-null caller-owned slot set to 1, 2, 3 or 4
 * on a recognized header; unchanged on reported open/format failure.
 * @return Zero for a recognized header, ERR_HOTSTART_FILE_OPEN on opening
 * failure, or ERR_HOTSTART_FILE_FORMAT for an unrecognized version-1 header.
 * @note This does not validate record counts, payload completeness or I/O
 * status of every header read. The local stream is closed before returning.
 */
int hotstart_is_valid(const char* hotstartFile, int* inputFileVersion)
//
//  Input:   hotStartFile     = filepath to hotstart file to check
//  Output:  inputFileVersion = version of hotstart file
//           returns error code
{
    char fStamp[] = "SWMM5-HOTSTART";
    char fileStamp[] = "SWMM5-HOTSTART";
    char fStampx[] = "SWMM5-HOTSTARTx";
    char fileStamp2[] = "SWMM5-HOTSTART2";
    char fileStamp3[] = "SWMM5-HOTSTART3";
    char fileStamp4[] = "SWMM5-HOTSTART4";
    FILE* f;

    // --- try to open the file
    if ((f = fopen(hotstartFile, "r+b")) == NULL) {
        return ERR_HOTSTART_FILE_OPEN;
    }

    // --- check that file contains proper header records
    fread(fStampx, sizeof(char), strlen(fileStamp2), f);
    if (strcmp(fStampx, fileStamp4) == 0)
        *inputFileVersion = 4;
    else if (strcmp(fStampx, fileStamp3) == 0)
        *inputFileVersion = 3;
    else if (strcmp(fStampx, fileStamp2) == 0)
        *inputFileVersion = 2;
    else {
        rewind(f);
        fread(fStamp, sizeof(char), strlen(fileStamp), f);
        if (strcmp(fStamp, fileStamp) != 0) {
            fclose(f);
            return ERR_HOTSTART_FILE_FORMAT;
        }
        *inputFileVersion = 1;
    }

    fclose(f);
    return 0;
}

//=============================================================================

/**
 * @brief Saves and closes hotstart files that are to be written at end of simulation.
 */
void hotstart_close() {
    int i;

    for (i = 0; i < MAXHOTSTARTFILES; i++) {
        if (FhotstartOutputs[i].file && FhotstartOutputs[i].saveDateTime == 0) {
            saveRunoff(&FhotstartOutputs[i]);
            saveRouting(&FhotstartOutputs[i]);
            fclose(FhotstartOutputs[i].file);
        }
    }
}

//=============================================================================

/**
 * @brief Restore global model state from the configured hotstart input.
 * @return TRUE if no hotstart input is configured or restoration finishes
 * without ErrorCode; FALSE after a header/count/unit mismatch or reported read error.
 * @details Opens FhotstartInput, verifies object counts and flow units,
 * reads runoff for version >= 3 and routing for all supported versions.
 * Reports errors through the global reporting channel. Success closes the
 * input stream; some header-mismatch paths leave it for later lifecycle cleanup.
 * @pre The project objects and hotstart input configuration are initialized.
 */
int initializeFromHotstartFile()
//
//  Input:   none
//  Output:  True for successful initialization of hotstart file or False otherwise
//  Purpose: initializes model state a previously saved routing hotstart file.
//
{
    int nSubcatch;
    int nNodes;
    int nLinks;
    int nPollut;
    int nLandUses;
    int flowUnits;
    int errorCode;

    // --- try to open the file
    if (FhotstartInput.mode != USE_FILE)
        return TRUE;
    errorCode = hotstart_is_valid(FhotstartInput.name, &fileVersion);

    if (errorCode) {
        if (errorCode == ERR_HOTSTART_FILE_OPEN) {
            report_writeErrorMsg(ERR_HOTSTART_FILE_OPEN, FhotstartInput.name);
        } else {
            report_writeErrorMsg(errorCode, "");
        }
        return FALSE;
    }

    FhotstartInput.file = fopen(FhotstartInput.name, "r+b");
    // --- read header records from the file
    if (fileVersion > 1) {
        char fStampx[] = "SWMM5-HOTSTARTx";
        char fileStamp2[] = "SWMM5-HOTSTART2";
        fread(fStampx, sizeof(char), strlen(fileStamp2), FhotstartInput.file);
    } else {
        char fStamp[] = "SWMM5-HOTSTART";
        char fileStamp[] = "SWMM5-HOTSTART";
        fread(fStamp, sizeof(char), strlen(fileStamp), FhotstartInput.file);
    }

    nSubcatch = -1;
    nNodes = -1;
    nLinks = -1;
    nPollut = -1;
    nLandUses = -1;
    flowUnits = -1;
    if (fileVersion >= 2) {
        fread(&nSubcatch, sizeof(int), 1, FhotstartInput.file);
    } else
        nSubcatch = Nobjects[SUBCATCH];
    if (fileVersion >= 3) {
        fread(&nLandUses, sizeof(int), 1, FhotstartInput.file);
    } else
        nLandUses = Nobjects[LANDUSE];
    fread(&nNodes, sizeof(int), 1, FhotstartInput.file);
    fread(&nLinks, sizeof(int), 1, FhotstartInput.file);
    fread(&nPollut, sizeof(int), 1, FhotstartInput.file);
    fread(&flowUnits, sizeof(int), 1, FhotstartInput.file);
    if (nSubcatch != Nobjects[SUBCATCH] || nLandUses != Nobjects[LANDUSE] ||
        nNodes != Nobjects[NODE] || nLinks != Nobjects[LINK] || nPollut != Nobjects[POLLUT] ||
        flowUnits != FlowUnits) {
        report_writeErrorMsg(ERR_HOTSTART_FILE_FORMAT, "");
        return FALSE;
    }

    // --- read contents of the file and close it
    if (fileVersion >= 3)
        readRunoff();
    readRouting();
    fclose(FhotstartInput.file);
    if (ErrorCode)
        return FALSE;
    else
        return TRUE;
}

//=============================================================================

/**
 * @brief Open and write the header for a version-4 hotstart destination.
 * @param[in,out] hotstartFile Non-null borrowed descriptor containing mode
 * and filename; SAVE_FILE opens/truncates the path and assigns its FILE pointer.
 * @return Zero when mode is not SAVE_FILE or the file opened successfully;
 * ERR_HOTSTART_FILE_OPEN if opening fails. Header fwrite errors are not checked.
 * @note Leaves a successful stream open for saveRunoff()/saveRouting(); the
 * caller owns closing it. Global object counts and FlowUnits form the header.
 */
int initializeSaveHotstartFile(TFile* hotstartFile)
//
//  Input:   hotStartFile = filepath to hotstart file to use
//  Output:  returns error code
//  Purpose: opens a new routing hotstart file to save results to.
//
{
    int error_code = 0;
    int nSubcatch;
    int nLandUses;
    int nNodes;
    int nLinks;
    int nPollut;
    int flowUnits;
    char fileStamp[] = "SWMM5-HOTSTART4";

    // --- try to open file
    if (hotstartFile->mode != SAVE_FILE)
        return 0;

    if ((hotstartFile->file = fopen(hotstartFile->name, "w+b")) == NULL) {
        error_code = ERR_HOTSTART_FILE_OPEN;
        return error_code;
    }

    // --- write file stamp & number of objects to file
    nSubcatch = Nobjects[SUBCATCH];
    nLandUses = Nobjects[LANDUSE];
    nNodes = Nobjects[NODE];
    nLinks = Nobjects[LINK];
    nPollut = Nobjects[POLLUT];
    flowUnits = FlowUnits;
    fwrite(fileStamp, sizeof(char), strlen(fileStamp), hotstartFile->file);
    fwrite(&nSubcatch, sizeof(int), 1, hotstartFile->file);
    fwrite(&nLandUses, sizeof(int), 1, hotstartFile->file);
    fwrite(&nNodes, sizeof(int), 1, hotstartFile->file);
    fwrite(&nLinks, sizeof(int), 1, hotstartFile->file);
    fwrite(&nPollut, sizeof(int), 1, hotstartFile->file);
    fwrite(&flowUnits, sizeof(int), 1, hotstartFile->file);
    return error_code;
}

//=============================================================================

/**
 * @brief Append current node/link hydraulic and quality state to a hotstart stream.
 * @param[in,out] hotstartFile Non-null borrowed descriptor with an open
 * writable binary stream positioned after the appropriate preceding records.
 * @note Reads initialized global model arrays and advances the stream;
 * does not flush or close it. Write errors are not checked by this routine.
 */
void saveRouting(TFile* hotstartFile)
//
//  Input:   hotStartFile = hotstart file to use
//  Output:  none
//  Purpose: saves current state of all nodes and links to hotstart file.
//
{
    int i, j;
    float x[3] = {0};

    for (i = 0; i < Nobjects[NODE]; i++) {
        x[0] = (float)Node[i].newDepth;
        x[1] = (float)Node[i].newLatFlow;
        fwrite(x, sizeof(float), 2, hotstartFile->file);

        if (Node[i].type == STORAGE) {
            j = Node[i].subIndex;
            x[0] = (float)Storage[j].hrt;
            fwrite(&x[0], sizeof(float), 1, hotstartFile->file);
        }

        for (j = 0; j < Nobjects[POLLUT]; j++) {
            x[0] = (float)Node[i].newQual[j];
            fwrite(&x[0], sizeof(float), 1, hotstartFile->file);
        }
    }
    for (i = 0; i < Nobjects[LINK]; i++) {
        x[0] = (float)Link[i].newFlow;
        x[1] = (float)Link[i].newDepth;
        x[2] = (float)Link[i].setting;
        fwrite(x, sizeof(float), 3, hotstartFile->file);
        for (j = 0; j < Nobjects[POLLUT]; j++) {
            x[0] = (float)Link[i].newQual[j];
            fwrite(&x[0], sizeof(float), 1, hotstartFile->file);
        }
    }
}

//=============================================================================

/**
 * @brief Reads initial state of all nodes, links and groundwater objects from hotstart file.
 */
void readRouting() {
    int i, j;
    float x;
    double xgw[4] = {0};
    FILE* f = FhotstartInput.file;

    // --- for file format 2, assign GW moisture content and lower depth
    if (fileVersion == 2) {
        // --- flow and available upper zone volume not used
        xgw[2] = 0.0;
        xgw[3] = MISSING;
        for (i = 0; i < Nobjects[SUBCATCH]; i++) {
            // --- read moisture content and water table elevation as floats
            if (!readFloat(&x, f))
                return;
            xgw[0] = x;
            if (!readFloat(&x, f))
                return;
            xgw[1] = x;

            // --- set GW state
            if (Subcatch[i].groundwater != NULL)
                gwater_setState(i, xgw);
        }
    }

    // --- read node states
    for (i = 0; i < Nobjects[NODE]; i++) {
        if (!readFloat(&x, f))
            return;
        Node[i].newDepth = x;
        if (!readFloat(&x, f))
            return;
        Node[i].newLatFlow = x;

        if (fileVersion >= 4 && Node[i].type == STORAGE) {
            if (!readFloat(&x, f))
                return;
            j = Node[i].subIndex;
            Storage[j].hrt = x;
        }

        for (j = 0; j < Nobjects[POLLUT]; j++) {
            if (!readFloat(&x, f))
                return;
            Node[i].newQual[j] = x;
        }

        // --- read in zeros here for backwards compatibility
        if (fileVersion <= 2) {
            for (j = 0; j < Nobjects[POLLUT]; j++) {
                if (!readFloat(&x, f))
                    return;
            }
        }
    }

    // --- read link states
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (!readFloat(&x, f))
            return;
        Link[i].newFlow = x;
        if (!readFloat(&x, f))
            return;
        Link[i].newDepth = x;
        if (!readFloat(&x, f))
            return;
        Link[i].setting = x;

        // --- set link's target setting to saved setting
        Link[i].targetSetting = x;
        link_setTargetSetting(i);
        link_setSetting(i, 0.0);

        for (j = 0; j < Nobjects[POLLUT]; j++) {
            if (!readFloat(&x, f))
                return;
            Link[i].newQual[j] = x;
        }
    }
}

//=============================================================================

/**
 * @brief Append current subcatchment runoff, infiltration, groundwater, snow and quality state to a hotstart stream.
 * @param[in,out] hotstartFile Non-null borrowed descriptor with an open
 * writable binary stream positioned after the appropriate preceding records.
 * @note Reads initialized global model arrays and advances the stream;
 * does not flush or close it. Write errors are not checked by this routine.
 */
void saveRunoff(TFile* hotstartFile)
//
//  Input:   hotStartFile = hotstart file to use
//  Output:  none
//  Purpose: saves current state of all subcatchments to hotstart file.
//
{
    int i, j, k;
    double x[6] = {0};
    FILE* f = hotstartFile->file;

    for (i = 0; i < Nobjects[SUBCATCH]; i++) {
        // Ponded depths for each sub-area & total runoff (4 elements)
        for (j = 0; j < 3; j++)
            x[j] = Subcatch[i].subArea[j].depth;
        x[3] = Subcatch[i].newRunoff;
        fwrite(x, sizeof(double), 4, f);

        // Infiltration state (max. of 6 elements)
        for (j = 0; j < 6; j++)
            x[j] = 0.0;
        infil_getState(i, x);
        fwrite(x, sizeof(double), 6, f);

        // Groundwater state (4 elements)
        if (Subcatch[i].groundwater != NULL) {
            gwater_getState(i, x);
            fwrite(x, sizeof(double), 4, f);
        }

        // Snowpack state (5 elements for each of 3 snow surfaces)
        if (Subcatch[i].snowpack != NULL) {
            for (j = 0; j < 3; j++) {
                snow_getState(i, j, x);
                fwrite(x, sizeof(double), 5, f);
            }
        }

        // Water quality
        if (Nobjects[POLLUT] > 0) {
            // Runoff quality
            for (j = 0; j < Nobjects[POLLUT]; j++) {
                x[0] = Subcatch[i].newQual[j];
                fwrite(x, sizeof(double), 1, f);
            }

            // Ponded quality
            for (j = 0; j < Nobjects[POLLUT]; j++) {
                x[0] = Subcatch[i].pondedQual[j];
                fwrite(x, sizeof(double), 1, f);
            }

            // Buildup and when streets were last swept
            for (k = 0; k < Nobjects[LANDUSE]; k++) {
                for (j = 0; j < Nobjects[POLLUT]; j++) {
                    x[0] = Subcatch[i].landFactor[k].buildup[j];
                    fwrite(x, sizeof(double), 1, f);
                }
                x[0] = Subcatch[i].landFactor[k].lastSwept;
                fwrite(x, sizeof(double), 1, f);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Reads saved state of all subcatchments from a hot start file.
 */
void readRunoff() {
    int i, j, k;
    double x[6] = {0};
    FILE* f = FhotstartInput.file;

    for (i = 0; i < Nobjects[SUBCATCH]; i++) {
        // Ponded depths & runoff (4 elements)
        for (j = 0; j < 3; j++) {
            if (!readDouble(&Subcatch[i].subArea[j].depth, f))
                return;
        }
        if (!readDouble(&Subcatch[i].newRunoff, f))
            return;

        // Infiltration state (max. of 6 elements)
        for (j = 0; j < 6; j++)
            if (!readDouble(&x[j], f))
                return;
        infil_setState(i, x);

        // Groundwater state (4 elements)
        if (Subcatch[i].groundwater != NULL) {
            for (j = 0; j < 4; j++)
                if (!readDouble(&x[j], f))
                    return;
            gwater_setState(i, x);
        }

        // Snowpack state (5 elements for each of 3 snow surfaces)
        if (Subcatch[i].snowpack != NULL) {
            for (j = 0; j < 3; j++) {
                for (k = 0; k < 5; k++)
                    if (!readDouble(&x[k], f))
                        return;
                snow_setState(i, j, x);
            }
        }

        // Water quality
        if (Nobjects[POLLUT] > 0) {
            // Runoff quality
            for (j = 0; j < Nobjects[POLLUT]; j++)
                if (!readDouble(&Subcatch[i].newQual[j], f))
                    return;

            // Ponded quality
            for (j = 0; j < Nobjects[POLLUT]; j++)
                if (!readDouble(&Subcatch[i].pondedQual[j], f))
                    return;

            // Buildup and when streets were last swept
            for (k = 0; k < Nobjects[LANDUSE]; k++) {
                for (j = 0; j < Nobjects[POLLUT]; j++) {
                    if (!readDouble(&Subcatch[i].landFactor[k].buildup[j], f))
                        return;
                }
                if (!readDouble(&Subcatch[i].landFactor[k].lastSwept, f))
                    return;
            }
        }
    }
}

//=============================================================================

/**
 * @brief Read one native float and reject a resulting NaN value.
 * @param[out] x Non-null caller-owned float slot. A detected NaN is replaced by zero.
 * @param[in,out] f Non-null borrowed readable binary stream, advanced by fread.
 * @return FALSE after detecting NaN and reporting ERR_HOTSTART_FILE_READ,
 * otherwise TRUE. The fread result is not checked, so TRUE alone does not
 * prove a complete value was read; short reads may leave x partly unchanged.
 * @note Does not close or retain the stream.
 */
int readFloat(float* x, FILE* f)
//
//  Input:   none
//  Output:  x  = pointer to a float variable
//  Purpose: reads a floating point value from a hot start file
//
{
    // --- read a value from the file
    fread(x, sizeof(float), 1, f);

    // --- test if the value is NaN (not a number)
    if (*(x) != *(x)) {
        report_writeErrorMsg(ERR_HOTSTART_FILE_READ, "");
        *(x) = 0.0;
        return FALSE;
    }
    return TRUE;
}

//=============================================================================

/**
 * @brief Read one native double with legacy EOF and NaN checks.
 * @param[out] x Non-null caller-owned slot; zeroed on preexisting EOF or detected NaN.
 * @param[in,out] f Non-null borrowed readable binary stream, advanced by fread.
 * @return FALSE for preexisting EOF or NaN; TRUE otherwise. The actual
 * fread count is unchecked, so a newly encountered short read may not be reported.
 * @note Preexisting EOF reports ERR_HOTSTART_FILE_READ; NaN rejection does
 * not report it here. The stream is neither closed nor retained.
 */
int readDouble(double* x, FILE* f)
//
//  Input:   none
//  Output:  x  = pointer to a double variable
//  Purpose: reads a floating point value from a hot start file
//
{
    // --- read a value from the file
    if (feof(f)) {
        *(x) = 0.0;
        report_writeErrorMsg(ERR_HOTSTART_FILE_READ, "");
        return FALSE;
    }
    fread(x, sizeof(double), 1, f);

    // --- test if the value is NaN (not a number)
    if (*(x) != *(x)) {
        *(x) = 0.0;
        return FALSE;
    }
    return TRUE;
}
