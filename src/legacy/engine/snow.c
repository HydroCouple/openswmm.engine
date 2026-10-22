//-----------------------------------------------------------------------------
//   snow.c
//
//   Project: EPA SWMM5
//   Version: 5.2
//   Date:    11/01/21   (Build 5.2.0)
//   Author:  L. Rossman
//
//   Models snow melt processes.
//
//   Update History
//   ==============
//   Build 5.1.008:
//   - Adjustment of snowmelt and subcatchment's net precipitation for area
//     covered by snow was corrected.
//   - Area covered by snow now included in calculation of rate that liquid
//     water leaves a snowpack.
//   Build 5.2.0:
//   - Subcatchment snow pack area should not include LID area.
//-----------------------------------------------------------------------------

/**
 * @file      snow.c
 * @brief     Legacy snow accumulation, removal, and melt calculations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "headers.h"

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
// These symbolize the keywords listed in SnowmeltWords in keywords.c
enum SnowKeywords { SNOW_PLOWABLE, SNOW_IMPERV, SNOW_PERV, SNOW_REMOVAL };

//-----------------------------------------------------------------------------
//  External functions (declared in funcs.h)
//-----------------------------------------------------------------------------
//  snow_createSnowpack  (called from subcatch_setParams)
//  snow_initSnowpack    (called from subcatch_initState)
//  snow_initSnowmelt    (called from createObjects in project.c)
//  snow_validateSnowmelt(called from project_validate)
//  snow_readMeltParams  (called from parseLine in input.c)
//  snow_setMeltCoeffs   (called from setTemp in climate.c)
//  snow_plowSnow        (called from runoff_execute)
//  snow_getSnowMelt     (called from subcatch_getRunoff)
//  snow_getSnowCover    (called from massbal_open)
//  snow_getState        (called from saveRunoff in hotstart.c)

//-----------------------------------------------------------------------------
//  Local functions
//-----------------------------------------------------------------------------
static void setMeltParams(int i, int k, double x[]);
static double getRainmelt(double rainfall);
static double getArealDepletion(TSnowpack* snowpack, int i, double snowfall, double tStep);
static double getArealSnowCover(int i, double awesi);
static double meltSnowpack(TSnowpack* snowpack, int i, double rmelt, double asc, double snowfall,
                           double tStep);
static double reduceColdContent(TSnowpack* snowpack, int i, double smelt, double ccFactor);
static double routeSnowmelt(TSnowpack* snowpack, int i, double smelt, double asc, double rainfall,
                            double tStep);
static void updateColdContent(TSnowpack* snowpack, int i, double asc, double snowfall,
                              double tStep);

/*!
* \brief Reads snowmelt parameters from a tokenized line of input data.
* @par Implementation arguments
 * - tok (in): Array of string tokens
* - ntoks (in): Number of tokens
* \return Error code
* \details
* Format of data are:
* - Name  SubArea   Cmin  Cmax  Tbase  FWF  SD0  FW0  SNN0/SD100
* - Name  REMOVAL   SDplow Fout Fimperv Fperv Fimelt Fsubcatch (Subcatch)
 *
 * @details Reads snow melt parameters from a tokenized line of input data. Format of data are:
 *          Name  SubArea   Cmin  Cmax  Tbase  FWF  SD0  FW0  SNN0/SD100 Name  REMOVAL   SDplow
 *          Fout Fimperv Fperv Fimelt Fsubcatch (Subcatch).
 */
int snow_readMeltParams(char* tok[], int ntoks) {
    int i, j, k, m, n;
    double x[7];
    if (ntoks < 8)
        return error_setInpError(ERR_ITEMS, "");

    // --- save snow melt parameter set name if not already done so
    j = project_findObject(SNOWMELT, tok[0]);
    if (j < 0)
        return error_setInpError(ERR_NAME, tok[0]);
    if (Snowmelt[j].ID == NULL)
        Snowmelt[j].ID = project_findID(SNOWMELT, tok[0]);

    // --- identify data keyword
    k = findmatch(tok[1], SnowmeltWords);
    if (k < 0)
        return error_setInpError(ERR_KEYWORD, tok[1]);

    // --- number of parameters to read
    n = 7; // 7 for subareas
    if (k == SNOW_REMOVAL)
        n = 6; // 6 for Removal
    if (ntoks < n + 2)
        return error_setInpError(ERR_ITEMS, "");
    for (i = 0; i < 7; i++)
        x[i] = 0.0;

    // --- parse each parameter
    for (i = 0; i < n; i++) {
        if (!getDouble(tok[i + 2], &x[i]))
            return error_setInpError(ERR_NUMBER, tok[i + 2]);
    }

    // --- parse name of subcatch receiving snow plowed from current subcatch
    if (k == SNOW_REMOVAL) {
        x[6] = -1.0;
        if (ntoks >= 9) {
            m = project_findObject(SUBCATCH, tok[8]);
            if (m < 0)
                return error_setInpError(ERR_NAME, tok[8]);
            x[6] = m;
        }
    }

    // --- save snow melt parameters
    setMeltParams(j, k, x);
    return 0;
}

/**
 * @brief Allocate a subcatchment snowpack and associate its snowmelt parameters.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index; no existing owned snowpack
 * may be overwritten without first releasing it.
 * - snowIndex (in): Valid snowmelt parameter-set index stored in the new pack.
 * @return TRUE after allocation and attachment, FALSE if allocation fails.
 * @note Only snowmeltIndex is initialized here. Call snow_initSnowpack()
 * before using state arrays; the project owns the allocated snowpack.
 */
int snow_createSnowpack(int subcatchIndex, int snowIndex)
//
//  Input:   j = subcatchment index
//           k = snow melt parameter set index
//  Output:  returns TRUE if successful
//  Purpose: creates a snowpack object for a subcacthment.
//
{
    TSnowpack* snowpack;
    snowpack = (TSnowpack*)malloc(sizeof(TSnowpack));
    if (!snowpack)
        return FALSE;
    Subcatch[subcatchIndex].snowpack = snowpack;
    snowpack->snowmeltIndex = snowIndex;
    return TRUE;
}

/**
 * @brief Initialize a subcatchment's three snow surfaces and aggregate snow depth.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index; a missing snowpack is a no-op.
 * @post Sets surface area fractions, snow/free-water depths, cold content,
 * antecedent-temperature index and depletion state from the selected Snowmelt
 * parameters, and writes Subcatch.newSnowDepth in feet of water equivalent.
 * @pre The pack's snowmeltIndex and subcatchment impervious fraction are valid.
 */
void snow_initSnowpack(int subcatchIndex)
//
//  Input:   j = subcatchment index
//  Output:  none
//  Purpose: initializes state of a subcatchment's snow pack.
//
{
    int i;                  // snow sub-area index
    int k;                  // snowmelt parameter set index
    double f;               // fraction of impervious area plowable
    double snowDepth = 0.0; // snow depth on entire subcatchment (ft)
    TSnowpack* snowpack;    // ptr. to snow pack object

    // --- get ptr. to subcatchment's snow pack object
    snowpack = Subcatch[subcatchIndex].snowpack;
    if (snowpack == NULL)
        return;

    // --- identify index of snow melt data set used by snow pack
    k = Subcatch[subcatchIndex].snowpack->snowmeltIndex;

    // --- find fractional area of each snow surface
    f = Snowmelt[k].snn;
    snowpack->fArea[SNOW_PLOWABLE] = f * Subcatch[subcatchIndex].fracImperv;
    snowpack->fArea[SNOW_IMPERV] = (1.0 - f) * Subcatch[subcatchIndex].fracImperv;
    snowpack->fArea[SNOW_PERV] = 1.0 - Subcatch[subcatchIndex].fracImperv;

    // --- initialize state of snow pack on each snow surface
    for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++) {
        if (snowpack->fArea[i] > 0.0) {
            snowpack->wsnow[i] = Snowmelt[k].wsnow[i];
            snowpack->fw[i] = Snowmelt[k].fwnow[i];
        } else {
            snowpack->wsnow[i] = 0.0;
            snowpack->fw[i] = 0.0;
        }
        snowpack->coldc[i] = 0.0;
        snowpack->ati[i] = Snowmelt[k].tbase[i];
        snowpack->awe[i] = 1.0;
        snowDepth += snowpack->wsnow[i] * snowpack->fArea[i];
    }
    Subcatch[subcatchIndex].newSnowDepth = snowDepth;
}

/**
 * @brief Set default values for a snowmelt parameter set.
 * @par Implementation arguments
 * - snowIndex (in): Valid destination index into Snowmelt.
 * @post Clears three-surface melt, water and redistribution properties,
 * sets the plowing trigger to 1e6 and clears the receiving-subcatchment reference.
 * @note Does not allocate the Snowmelt array or initialize snowpack objects.
 */
void snow_initSnowmelt(int snowIndex)
//
//  Input:   j = snowmelt parameter set index
//  Output:  none
//  Purpose: initializes values in a snow melt parameter set.
//
{
    int i, k;
    for (i = 0; i < 3; i++) {
        Snowmelt[snowIndex].snn = 0.0;
        Snowmelt[snowIndex].si[i] = 0.0;
        Snowmelt[snowIndex].dhmin[i] = 0.0;
        Snowmelt[snowIndex].dhmax[i] = 0.0;
        Snowmelt[snowIndex].tbase[i] = 0.0;
        Snowmelt[snowIndex].fwfrac[i] = 0.0;
        Snowmelt[snowIndex].wsnow[i] = 0.0;
        Snowmelt[snowIndex].fwnow[i] = 0.0;
        Snowmelt[snowIndex].weplow = 1.0e6;
        for (k = 0; k < 5; k++)
            Snowmelt[snowIndex].sfrac[k] = 0.0;
        Snowmelt[snowIndex].toSubcatch = -1;
    }
}

/**
 * @brief Check melt coefficient ordering and fractional snowpack parameters.
 * @par Implementation arguments
 * - snowIndex (in): Valid parameter-set index into Snowmelt.
 * @post Reports ERR_SNOWPACK_PARAMS if min melt coefficients exceed maxima,
 * free-water or plowable fractions lie outside [0, 1], or redistribution
 * fractions sum above 1.01. Does not repair parameter values.
 */
void snow_validateSnowmelt(int snowIndex)
//
//  Input:   j = snowmelt parameter set index
//  Output:  none
//  Purpose: checks for valid values in a snow melt parameter set.
//
{
    int k;
    char err = FALSE;
    double sum = 0.0;

    for (k = SNOW_PLOWABLE; k <= SNOW_PERV; k++) {
        // --- check melt coeffs.
        if (Snowmelt[snowIndex].dhmin[k] > Snowmelt[snowIndex].dhmax[k])
            err = TRUE;

        // --- check free water fraction
        if (Snowmelt[snowIndex].fwfrac[k] < 0.0 || Snowmelt[snowIndex].fwfrac[k] > 1.0)
            err = TRUE;
    }

    // --- check fraction of imperv. area plowable
    if (Snowmelt[snowIndex].snn < 0.0 || Snowmelt[snowIndex].snn > 1.0)
        err = TRUE;

    // --- check that removal fractions sum <= 1.0
    for (k = 0; k < 5; k++)
        sum += Snowmelt[snowIndex].sfrac[k];
    if (sum > 1.01)
        err = TRUE;
    if (err)
        report_writeErrorMsg(ERR_SNOWPACK_PARAMS, Snowmelt[snowIndex].ID);
}

/**
 * @brief Copy one snow surface's five-component hotstart state.
 * @par Implementation arguments
 * - i (in): Valid subcatchment index; a missing snowpack leaves x unchanged.
 * - j (in): Snow surface index SNOW_PLOWABLE..SNOW_PERV, in [0, 2].
 * - x (out): Borrowed writable array of at least five doubles: snow water
 * equivalent depth, free water, cold content, antecedent temperature and
 * initial depletion-curve AWESI. Depths use feet and temperature uses degrees F.
 * @note Preserves the internal cold-content/depletion representation for
 * snow_setState(); no conversion, validation or storage retention occurs.
 */
void snow_getState(int i, int j, double x[])
//
//  Input:   i = subcatchment index
//           j = snow pack sub-area index
//  Output:  updates array of snow pack state variables x
//  Purpose: retrieves the current state of a snow pack object.
//
{
    TSnowpack* snowpack = Subcatch[i].snowpack;
    if (snowpack == NULL)
        return;
    x[0] = snowpack->wsnow[j];
    x[1] = snowpack->fw[j];
    x[2] = snowpack->coldc[j];
    x[3] = snowpack->ati[j];
    x[4] = snowpack->awe[j];
}

/**
 * @brief Restore one snow surface's five-component hotstart state.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index; missing snowpack is a no-op.
 * - subAreaIndex (in): Snow surface index SNOW_PLOWABLE..SNOW_PERV, in [0, 2].
 * - x (in): Borrowed readable array of at least five doubles in exactly
 * the order/internal units returned by snow_getState().
 * @post Copies all five values without validation; aggregate subcatchment
 * snow depth is not recomputed here. Input storage is not retained.
 */
void snow_setState(int subcatchIndex, int subAreaIndex, double x[])
//
//  Input:   i = subcatchment index
//           j = snow pack sub-area index
//           x = array of snow pack state variables
//  Output:  none
//  Purpose: sets the current state of a snow pack object.
//
{
    TSnowpack* snowpack = Subcatch[subcatchIndex].snowpack;
    if (snowpack == NULL)
        return;
    snowpack->wsnow[subAreaIndex] = x[0];
    snowpack->fw[subAreaIndex] = x[1];
    snowpack->coldc[subAreaIndex] = x[2];
    snowpack->ati[subAreaIndex] = x[3];
    snowpack->awe[subAreaIndex] = x[4];
}

//=============================================================================

/**
 * @brief Assigns values to parameters in a snow melt data set.
 *
 * @param[in] j Snowmelt parameter set index.
 * @param[in] k Data category index.
 * @param x Array of snow parameter values.
 */
void setMeltParams(int j, int k, double x[]) {
    int i;

    // --- snow pack melt parameters
    if (k >= SNOW_PLOWABLE && k <= SNOW_PERV) {
        // --- min/max melt coeffs.
        Snowmelt[j].dhmin[k] = x[0] * UCF(TEMPERATURE) / UCF(RAINFALL);
        Snowmelt[j].dhmax[k] = x[1] * UCF(TEMPERATURE) / UCF(RAINFALL);

        // --- base melt temp (deg F)
        Snowmelt[j].tbase[k] = x[2];
        if (UnitSystem == SI)
            Snowmelt[j].tbase[k] = (9. / 5.) * Snowmelt[j].tbase[k] + 32.0;

        // --- free water fractions
        Snowmelt[j].fwfrac[k] = x[3];

        // --- initial snow depth & free water depth
        Snowmelt[j].wsnow[k] = x[4] / UCF(RAINDEPTH);
        x[5] = MIN(x[5], (x[3] * x[4]));
        Snowmelt[j].fwnow[k] = x[5] / UCF(RAINDEPTH);

        // --- fraction of impervious area that is plowable
        if (k == SNOW_PLOWABLE)
            Snowmelt[j].snn = x[6];

        // --- min. depth for 100% areal coverage on remaining
        //     impervious area or total pervious area
        else
            Snowmelt[j].si[k] = x[6] / UCF(RAINDEPTH);
    }

    // --- removal parameters
    else if (k == SNOW_REMOVAL) {
        Snowmelt[j].weplow = x[0] / UCF(RAINDEPTH);
        for (i = 0; i <= 4; i++)
            Snowmelt[j].sfrac[i] = x[i + 1];
        if (x[6] >= 0.0)
            Snowmelt[j].toSubcatch = (int)(x[6] + 0.01);
        else
            Snowmelt[j].toSubcatch = -1;
    }
}

/**
 * @brief Interpolate all three surfaces' seasonal melt coefficients.
 * @par Implementation arguments
 * - snowIndex (in): Valid Snowmelt parameter-set index.
 * - season (in): Dimensionless seasonal factor, normally in [-1, 1];
 * -1 selects dhmin and +1 selects dhmax. Values are not clamped.
 * @post Stores interpolated dhm coefficients in the parameter set's internal units.
 */
void snow_setMeltCoeffs(int snowIndex, double season)
//
//  Input:   j = snowmelt parameter set index
//           s = snow season of year
//  Output:  none
//  Purpose: sets values of snow melt coeffs. for particular time of year.
//
{
    int k; // snow sub-area index

    for (k = SNOW_PLOWABLE; k <= SNOW_PERV; k++) {
        Snowmelt[snowIndex].dhm[k] = 0.5 * (Snowmelt[snowIndex].dhmax[k] * (1.0 + season) +
                                            Snowmelt[snowIndex].dhmin[k] * (1.0 - season));
    }
}

/**
 * @brief Accumulate snowfall and redistribute/plow snow above the trigger depth.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index; missing snowpack is a no-op.
 * - tStep (in): Positive accumulation/plowing interval in seconds.
 * @post Updates snow water-equivalent depths, immediate-melt rates,
 * Snow.removed and any receiving subcatchment pack. Gage snowfall is scaled
 * by snowScaleFactor and API snowfall is added before accumulation.
 * @pre Snowmelt redistribution targets and area fractions are validated.
 */
void snow_plowSnow(int subcatchIndex, double tStep)
//
//  Input:   j     = subcatchment index
//           tStep = time step (sec)
//  Output:  none
//  Purpose: adds new snow to subcatchment and plows it between sub-areas.
//
{
    int i;               // snow sub-area index
    int k;               // snowmelt parameter set index
    int m;               // subcatchment index
    double rainfall;     // rainfall (not used)
    double snowfall;     // snowfall (ft/sec)
    double exc;          // excess snow depth (ft)
    double f;            // area ratio
    double sfracTotal;   // total fraction of snow moved
    TSnowpack* snowpack; // ptr. to snow pack object

    snowpack = Subcatch[subcatchIndex].snowpack;
    if (!snowpack)
        return;

    // --- see if there's any snowfall
    gage_getPrecip(Subcatch[subcatchIndex].gage, &rainfall, &snowfall);

    // --- apply the subcatchment snow scale factor to the gage-derived
    //     component. This MUST match the scaling done in getNetPrecip()
    //     (subcatch.c) -- accumulation here and melt there consume the same
    //     snowfall value, and if they disagree the snow pack mass balance
    //     will not close.
    snowfall *= Subcatch[subcatchIndex].snowScaleFactor;

    // --- include any API prescribed snowfall so prescribed snow
    //     accumulates in the pack (previously it only influenced melt
    //     computations, leaving a runoff continuity hole)
    snowfall += Subcatch[subcatchIndex].apiSnowfall;

    // --- add snowfall to snow pack
    for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++) {
        if (snowpack->fArea[i] > 0.0) {
            snowpack->wsnow[i] += snowfall * tStep;
            snowpack->imelt[i] = 0.0;
        }
    }

    // --- see if there is excess snow on plowable area to remove
    if (snowpack->fArea[SNOW_PLOWABLE] > 0.0) {
        k = snowpack->snowmeltIndex;
        if (snowpack->wsnow[SNOW_PLOWABLE] >= Snowmelt[k].weplow) {
            // --- excess snow to be reomoved
            exc = snowpack->wsnow[SNOW_PLOWABLE];

            // --- plow out of system
            f = snowpack->fArea[SNOW_PLOWABLE] *
                (Subcatch[subcatchIndex].area - Subcatch[subcatchIndex].lidArea);
            Snow.removed += Snowmelt[k].sfrac[0] * exc * f;
            sfracTotal = Snowmelt[k].sfrac[0];

            // --- plow onto non-plowable impervious area
            if (snowpack->fArea[SNOW_IMPERV] > 0.0) {
                f = snowpack->fArea[SNOW_PLOWABLE] / snowpack->fArea[SNOW_IMPERV];
                snowpack->wsnow[SNOW_IMPERV] += Snowmelt[k].sfrac[1] * exc * f;
                sfracTotal += Snowmelt[k].sfrac[1];
            }

            // --- plow onto pervious area
            if (snowpack->fArea[SNOW_PERV] > 0.0) {
                f = snowpack->fArea[SNOW_PLOWABLE] / snowpack->fArea[SNOW_PERV];
                snowpack->wsnow[SNOW_PERV] += Snowmelt[k].sfrac[2] * exc * f;
                sfracTotal += Snowmelt[k].sfrac[2];
            }

            // --- convert to immediate melt
            snowpack->imelt[SNOW_PLOWABLE] = Snowmelt[k].sfrac[3] * exc / tStep;
            sfracTotal += Snowmelt[k].sfrac[3];

            // --- send to another subcatchment
            if (Snowmelt[k].sfrac[4] > 0.0) {
                m = Snowmelt[k].toSubcatch;
                if (Subcatch[m].snowpack) {
                    f = Subcatch[m].snowpack->fArea[SNOW_PERV];
                } else
                    f = 0.0;
                if (f > 0.0) {
                    f = snowpack->fArea[SNOW_PLOWABLE] / f;
                    Subcatch[m].snowpack->wsnow[SNOW_PERV] += Snowmelt[k].sfrac[4] * exc * f;
                    sfracTotal += Snowmelt[k].sfrac[4];
                }
            }

            // --- reduce snow depth by amount plowed
            sfracTotal = MIN(sfracTotal, 1.0);
            snowpack->wsnow[SNOW_PLOWABLE] = exc * (1.0 - sfracTotal);
        }
    }
}

/**
 * @brief Melt and route a snowpack and supply liquid precipitation to runoff areas.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index with a non-null initialized snowpack.
 * - rainfall (in): Liquid rainfall rate in feet per second.
 * - snowfall (in): Snowfall water-equivalent rate in feet per second.
 * - tStep (in): Positive melt/routing interval in seconds.
 * - netPrecip (out): Borrowed writable array of at least three doubles,
 * receiving rainfall plus melt in ft/s for the runoff subareas. Impervious
 * snow-surface rates are combined into IMPERV0/IMPERV1 entries.
 * @return Area-weighted snow water-equivalent depth in feet.
 * @post Advances the selected pack's snow, free-water, cold-content,
 * depletion and immediate-melt state; no output storage is retained.
 */
double snow_getSnowMelt(int subcatchIndex, double rainfall, double snowfall, double tStep,
                        double netPrecip[])
//
//  Input:   j = subcatchment index
//           rainfall = rainfall (ft/sec)
//           snowfall = snowfall (ft/sec)
//           tStep = time step (sec)
//  Output:  netPrecip = rainfall + snowmelt on each runoff sub-area (ft/sec),
//           returns new snow depth over subcatchment
//  Purpose: modifies rainfall input to subcatchment's sub-areas based on
//           possible snow melt and updates snow depth over entire subcatchment.
//
{
    int i;                  // snow sub-area index
    double rmelt;           // melt rate when rain falling (ft/sec)
    double smelt;           // snow melt from sub-area (ft/sec)
    double asc;             // frac. of sub-area snow covered
    double snowDepth = 0.0; // snow depth on entire subcatchment (ft)
    double impervPrecip;    // net precip. on imperv. area (ft/sec)
    TSnowpack* snowpack;    // ptr. to snow pack object

    // --- get ptr. to subcatchment's snowpack
    snowpack = Subcatch[subcatchIndex].snowpack;

    // --- compute snowmelt over entire subcatchment when rain falling
    rmelt = getRainmelt(rainfall);

    // --- compute snow melt from each type of subarea
    for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++) {
        // --- completely melt pack if its depth is < 0.001 inch
        if (snowpack->wsnow[i] <= 0.001 / 12.0) {
            asc = 0.0;
            smelt = 0.0;
            snowpack->imelt[i] += (snowpack->wsnow[i] + snowpack->fw[i]) / tStep;
            snowpack->wsnow[i] = 0.0;
            snowpack->fw[i] = 0.0;
            snowpack->coldc[i] = 0.0;
        }

        // --- otherwise compute areal depletion, find snow melt
        //     and route it through pack
        else {
            asc = getArealDepletion(snowpack, i, snowfall, tStep);
            smelt = meltSnowpack(snowpack, i, rmelt, asc, snowfall, tStep);
            smelt = routeSnowmelt(snowpack, i, smelt, asc, rainfall, tStep);
        }

        // --- find net precip. over entire subcatch area
        netPrecip[i] = smelt + snowpack->imelt[i] // snow pack melt
                       + rainfall * (1.0 - asc);  // rainfall on non-snow area

        // --- add to total snow depth on subcatchment
        snowDepth += snowpack->wsnow[i] * snowpack->fArea[i];
    }

    // --- combine netPrecip on plowable & non-plowable imperv. areas
    if (Subcatch[subcatchIndex].fracImperv > 0.0) {
        impervPrecip = (netPrecip[SNOW_PLOWABLE] * snowpack->fArea[SNOW_PLOWABLE] +
                        netPrecip[SNOW_IMPERV] * snowpack->fArea[SNOW_IMPERV]) /
                       Subcatch[subcatchIndex].fracImperv;
        netPrecip[IMPERV0] = impervPrecip;
        netPrecip[IMPERV1] = impervPrecip;
    }
    return snowDepth;
}

/**
 * @brief Sum stored snow and free-water volume over a subcatchment's non-LID area.
 * @par Implementation arguments
 * - subcatchIndex (in): Valid subcatchment index.
 * @return Snowpack water-equivalent volume in cubic feet, or zero when no
 * snowpack exists. Reads all three snow-surface area fractions without modifying state.
 */
double snow_getSnowCover(int subcatchIndex)
//
//  Input:   j = subcatchment index
//  Output:  returns volume of snow cover (ft3)
//  Purpose: computes volume of snow on a subcatchment.
//
{
    int i;
    double snowCover = 0.0; // snow cover volume (ft3)
    TSnowpack* snowpack;    // ptr. to snowpack object

    snowpack = Subcatch[subcatchIndex].snowpack;
    if (!snowpack)
        return 0.0;
    for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++) {
        snowCover += (snowpack->wsnow[i] + snowpack->fw[i]) * snowpack->fArea[i];
    }
    return snowCover * (Subcatch[subcatchIndex].area - Subcatch[subcatchIndex].lidArea);
}

//=============================================================================

/**
 * @brief Depletes snow covered area as snow pack melts.
 *
 * @param snowpack Ptr. to snow pack object.
 * @param[in] i Snow sub-area index.
 * @param[in] snowfall Snow fall rate (ft/sec).
 * @param[in] tStep Time step (sec).
 * @return Fraction of sub-area with snow cover.
 */
double getArealDepletion(TSnowpack* snowpack, int i, double snowfall, double tStep) {
    int k;        // index of snow melt parameter set
    double asc;   // fraction of area with 100% cover
    double si;    // snow depth at 100% cover
    double awesi; // depth relative to depth at 100% cover
    double awe;
    double sba;
    double sbws;

    // --- plowable sub-area not subject to areal depletion
    if (i == SNOW_PLOWABLE)
        return 1.0;
    k = snowpack->snowmeltIndex;
    si = Snowmelt[k].si[i];

    // --- no depletion if depth zero or above SI
    if (si == 0.0 || snowpack->wsnow[i] >= si) {
        snowpack->awe[i] = 1.0;
        return 1.0;
    }
    if (snowpack->wsnow[i] == 0.0) {
        snowpack->awe[i] = 1.0;
        return 0.0;
    }

    // --- case of new snowfall
    if (snowfall > 0.0) {
        awe = (snowpack->wsnow[i] - snowfall * tStep) / si;
        awe = MAX(awe, 0.0);
        sba = getArealSnowCover(i, awe);
        sbws = awe + (0.75 * snowfall * tStep) / si;
        sbws = MIN(sbws, 1.0);
        snowpack->awe[i] = awe;
        snowpack->sba[i] = sba;
        snowpack->sbws[i] = sbws;
        return 1.0;
    }

    // --- case of no new snow
    else {
        awe = snowpack->awe[i];
        sba = snowpack->sba[i];
        sbws = snowpack->sbws[i];
        awesi = snowpack->wsnow[i] / si;

        // --- relative snow depth is below start of new snow ADC
        if (awesi < snowpack->awe[i]) {
            snowpack->awe[i] = 1.0;
            asc = getArealSnowCover(i, awesi);
        }

        // --- relative snow depth is above end of new snow ADC
        else if (awesi >= snowpack->sbws[i]) {
            asc = 1.0;
        }

        // --- relative snow depth is on new snow ADC
        else {
            asc = sba + (1.0 - sba) / (sbws - awe) * (awesi - awe);
        }
        return asc;
    }
}

//=============================================================================

/**
 * @brief Finds x-value on areal depletion curve (ADC) for given y-value.
 *
 * @param[in] i Snow sub-area index.
 * @param[in] awesi Snow depth relative to depth at 100% snow cover.
 * @return Fraction of sub-area with snow cover.
 * @note Areal depletion curves are associated with a project's Snow data structure. They plot
 *       relative snow depth (awesi) as a function of snow covererd area fraction (asc) in 10 equal
 *       awesi increments between 0 and 1.0.
 */
double getArealSnowCover(int i, double awesi) {
    int k;             // type of ADC (impervious or pervious)
    int m;             // interval on ADC
    double asc1, asc2; // asc values at ends of interval

    // --- determine which ADC to use
    if (i == SNOW_IMPERV)
        k = 0;
    else if (i == SNOW_PERV)
        k = 1;
    else
        return 1.0;

    // --- locate interval on ADC that bounds awesi
    if (awesi <= 0.0)
        return 0.0;
    if (awesi >= 0.9999)
        return 1.0;
    m = (int)(awesi * 10.0 + 0.00001);

    // --- get asc values for either end of interval
    asc1 = Snow.adc[k][m];
    if (m >= 9)
        asc2 = 1.0;
    else
        asc2 = Snow.adc[k][m + 1];

    // --- return with interpolated asc value
    return asc1 + (asc2 - asc1) / 0.1 * (awesi - 0.1 * (float)m);
}

//=============================================================================

/**
 * @brief Computes rate of snow melt from snow sub-area.
 *
 * @param snowpack Ptr. to snow pack object.
 * @param[in] i Snow sub-area index.
 * @param[in] rmelt Melt rate if raining (ft/sec).
 * @param[in] asc Fraction of area covered with snow.
 * @param[in] snowfall Rate of snow fall (ft/sec).
 * @param[in] tStep Time step (sec).
 * @return Snow melt rate (ft/sec).
 */
double meltSnowpack(TSnowpack* snowpack, int i, double rmelt, double asc, double snowfall,
                    double tStep) {
    int k;           // snowmelt parameter set index
    double smelt;    // melt rate over sub-area (ft/sec)
    double ccFactor; // cold content conversion factor

    // --- if raining then use result found from getRainMelt
    k = snowpack->snowmeltIndex;
    if (rmelt > 0.0)
        smelt = rmelt;

    // --- else if air temp. >= base melt temp. then use degree-day eqn.
    else if (Temp.ta >= Snowmelt[k].tbase[i]) {
        smelt = Snowmelt[k].dhm[i] * (Temp.ta - Snowmelt[k].tbase[i]);
    }

    // --- otherwise alter cold content and return 0
    else {
        updateColdContent(snowpack, i, asc, snowfall, tStep);
        return 0.0;
    }

    // --- adjust snowmelt for area of snow cover
    smelt *= asc;

    // --- reduce cold content of melting pack
    ccFactor = tStep * Snow.rnm * asc;
    smelt = reduceColdContent(snowpack, i, smelt, ccFactor);
    snowpack->ati[i] = Snowmelt[k].tbase[i];
    return smelt;
}

//=============================================================================

/**
 * @brief Computes rate of snow melt when rainfall occurs.
 *
 * @param[in] rainfall Rainfall rate (ft/sec).
 * @return Snow melt rate (ft/sec).
 */
double getRainmelt(double rainfall) {
    double uadj; // adjusted wind speed
    double t1, t2, t3;
    double smelt; // snow melt in in/hr

    rainfall = rainfall * 43200.0; // convert rain to in/hr
    if (rainfall > 0.02) {
        uadj = 0.006 * Wind.ws;
        t1 = Temp.ta - 32.0;
        t2 = 7.5 * Temp.gamma * uadj;
        t3 = 8.5 * uadj * (Temp.ea - 0.18);
        smelt = t1 * (0.001167 + t2 + 0.007 * rainfall) + t3;
        return smelt / 43200.0;
    } else
        return 0.0;
}

//=============================================================================

/**
 * @brief Updates cold content of snow pack under non-melting conditions.
 *
 * @param snowpack Ptr. to snow pack object.
 * @param[in] i Snow sub-area index.
 * @param[in] asc Fraction of area snow covered.
 * @param[in] snowfall Snow fall rate (ft/sec).
 * @param[in] tStep Time step (sec).
 */
void updateColdContent(TSnowpack* snowpack, int i, double asc, double snowfall, double tStep) {
    int k;        // snowmelt parameter set index
    double ati;   // antecdent temperature index (deg F)
    double cc;    // snow pack cold content (ft)
    double ccMax; // max. possible cold content (ft)
    double tipm;  // adjusted ATI weighting factor

    // --- retrieve ATI & CC from snow pack object
    ati = snowpack->ati[i];
    cc = snowpack->coldc[i];

    // --- if snowing, ATI = snow (air) temperature
    if (snowfall * 43200.0 > 0.02)
        ati = Temp.ta;
    else {
        // convert ATI weighting factor from 6-hr to tStep time basis
        tipm = 1.0 - pow(1.0 - Snow.tipm, tStep / (6.0 * 3600.0));

        // update ATI
        ati += tipm * (Temp.ta - ati);
    }

    // --- ATI cannot exceed snow melt base temperature
    k = snowpack->snowmeltIndex;
    ati = MIN(ati, Snowmelt[k].tbase[i]);

    // --- update cold content
    cc += Snow.rnm * Snowmelt[k].dhm[i] * (ati - Temp.ta) * tStep * asc;
    cc = MAX(cc, 0.0);

    // --- maximum cold content based on assumed specific heat of snow
    //     of 0.007 in. water equiv. per deg. F
    ccMax = snowpack->wsnow[i] * 0.007 / 12.0 * (Snowmelt[k].tbase[i] - ati);
    cc = MIN(cc, ccMax);

    // --- assign updated values to snowpack
    snowpack->coldc[i] = cc;
    snowpack->ati[i] = ati;
}

//=============================================================================

/**
 * @brief Reduces cold content of snow pack adjusting melt rate accordingly.
 *
 * @param snowpack Ptr. to snowpack object.
 * @param[in] i Snow sub-area index.
 * @param[in] smelt Potential melt rate (ft/sec).
 * @param[in] ccFactor Cold content conversion factor.
 * @return Snow melt rate (ft/sec).
 */
double reduceColdContent(TSnowpack* snowpack, int i, double smelt, double ccFactor) {
    double cc; // cold content of snow pack (ft)

    cc = snowpack->coldc[i];
    if (smelt * ccFactor > cc) {
        smelt -= cc / ccFactor;
        cc = 0.0;
    } else {
        cc -= smelt * ccFactor;
        smelt = 0.0;
    }
    snowpack->coldc[i] = cc;
    return smelt;
}

//=============================================================================

/**
 * @brief Routes snow melt through free water holding capacity of snow pack.
 *
 * @param snowpack Ptr. to snowpack object.
 * @param[in] i Snow sub-area index.
 * @param[in] smelt Snow melt rate (ft/sec).
 * @param[in] asc Fraction of area snow covered.
 * @param[in] rainfall Rainfall rate (ft/sec).
 * @param[in] tStep Time step (sec).
 * @return Rate of liquid snow melt leaving a snow pack (ft/sec).
 */
double routeSnowmelt(TSnowpack* snowpack, int i, double smelt, double asc, double rainfall,
                     double tStep) {
    int k;        // snowmelt parameter set index
    double vmelt; // snow melt volume (ft)

    // --- get volume of snowmelt over time step
    k = snowpack->snowmeltIndex;
    vmelt = smelt * tStep;
    vmelt = MIN(vmelt, snowpack->wsnow[i]);

    // --- reduce snow depth by volume of snowmelt
    snowpack->wsnow[i] -= vmelt;

    // --- add snowmelt volume and any rainfall on snow
    //     covered area of sub-area to snow pack's free water content
    snowpack->fw[i] += vmelt + rainfall * tStep * asc;

    // --- excess free water becomes liquid melt that leaves the pack
    vmelt = snowpack->fw[i] - Snowmelt[k].fwfrac[i] * snowpack->wsnow[i];
    vmelt = MAX(vmelt, 0.0);

    // --- reduce free water by liquid melt volume and return liquid melt rate
    snowpack->fw[i] -= vmelt;
    return vmelt / tStep;
}

//=============================================================================
