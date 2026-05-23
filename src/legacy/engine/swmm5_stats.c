/*! \file swmm5_stats.c
 * \brief Extension API functions for accessing SWMM5 statistics and mass balance totals.
 * \author OpenSWMMCore contributors
 * \date Created on: 2026-03-25
 *
 * \details This file extends the legacy SWMM 5.x API (swmm5.c) with functions
 *          that expose per-element statistics and system-level mass balance
 *          breakdowns.  It accesses internal globals declared in stats.c and
 *          massbal.c via extern linkage.
 *
 *          Functions in this file should be called after swmm_end() but before
 *          swmm_close(), as the statistics arrays are freed during close.
 *
 *          NOTE: This file is an *addition* to the legacy engine — the original
 *          EPA SWMM 5.x source files are NOT modified.
 */

#include <stdlib.h>
#include <string.h>
#include "headers.h"
#include "openswmm_solver.h"

/* ====================================================================
 *  Extern declarations for statistics arrays (defined in stats.c)
 * ==================================================================== */
extern TSubcatchStats* SubcatchStats;
extern TNodeStats*     NodeStats;
extern TLinkStats*     LinkStats;
extern TStorageStats*  StorageStats;
extern TOutfallStats*  OutfallStats;
extern TPumpStats*     PumpStats;

/* ====================================================================
 *  Extern declarations for mass balance totals (defined in massbal.c)
 * ==================================================================== */
extern TRunoffTotals   RunoffTotals;
extern TRoutingTotals  FlowTotals;

/* ====================================================================
 *  Subcatchment Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSubcatchStats(
    int index, swmm_SubcatchStats *stats)
{
    if (SubcatchStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[SUBCATCH])
        return ERR_API_OBJECT_INDEX;

    stats->precip       = SubcatchStats[index].precip;
    stats->runon        = SubcatchStats[index].runon;
    stats->evap         = SubcatchStats[index].evap;
    stats->infil        = SubcatchStats[index].infil;
    stats->runoff       = SubcatchStats[index].runoff;
    stats->maxFlow      = SubcatchStats[index].maxFlow;
    stats->impervRunoff = SubcatchStats[index].impervRunoff;
    stats->pervRunoff   = SubcatchStats[index].pervRunoff;
    return 0;
}

/* ====================================================================
 *  Node Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getNodeStats(
    int index, swmm_NodeStats *stats)
{
    if (NodeStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[NODE])
        return ERR_API_OBJECT_INDEX;

    stats->avgDepth            = NodeStats[index].avgDepth;
    stats->maxDepth            = NodeStats[index].maxDepth;
    stats->maxDepthDate        = NodeStats[index].maxDepthDate;
    stats->maxRptDepth         = NodeStats[index].maxRptDepth;
    stats->volFlooded          = NodeStats[index].volFlooded;
    stats->timeFlooded         = NodeStats[index].timeFlooded;
    stats->timeSurcharged      = NodeStats[index].timeSurcharged;
    stats->timeCourantCritical = NodeStats[index].timeCourantCritical;
    stats->totLatFlow          = NodeStats[index].totLatFlow;
    stats->maxLatFlow          = NodeStats[index].maxLatFlow;
    stats->maxInflow           = NodeStats[index].maxInflow;
    stats->maxOverflow         = NodeStats[index].maxOverflow;
    stats->maxPondedVol        = NodeStats[index].maxPondedVol;
    stats->maxInflowDate       = NodeStats[index].maxInflowDate;
    stats->maxOverflowDate     = NodeStats[index].maxOverflowDate;
    return 0;
}

/* ====================================================================
 *  Storage Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getStorageStats(
    int index, swmm_StorageStats *stats)
{
    int j;

    if (StorageStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[NODE])
        return ERR_API_OBJECT_INDEX;

    /* Find the storage sub-index for this node */
    j = Node[index].subIndex;
    if (Node[index].type != STORAGE || j < 0 || j >= Nnodes[STORAGE])
        return ERR_API_OBJECT_TYPE;

    stats->initVol     = StorageStats[j].initVol;
    stats->avgVol      = StorageStats[j].avgVol;
    stats->maxVol      = StorageStats[j].maxVol;
    stats->maxFlow     = StorageStats[j].maxFlow;
    stats->evapLosses  = StorageStats[j].evapLosses;
    stats->exfilLosses = StorageStats[j].exfilLosses;
    stats->maxVolDate  = StorageStats[j].maxVolDate;
    return 0;
}

/* ====================================================================
 *  Outfall Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getOutfallStats(
    int index, swmm_OutfallStats *stats)
{
    int j, p;

    if (OutfallStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[NODE])
        return ERR_API_OBJECT_INDEX;

    /* Find the outfall sub-index for this node */
    j = Node[index].subIndex;
    if (Node[index].type != OUTFALL || j < 0 || j >= Nnodes[OUTFALL])
        return ERR_API_OBJECT_TYPE;

    stats->avgFlow      = OutfallStats[j].avgFlow;
    stats->maxFlow      = OutfallStats[j].maxFlow;
    stats->totalPeriods = OutfallStats[j].totalPeriods;

    /* Copy pollutant loads if array provided */
    if (stats->totalLoad != NULL && OutfallStats[j].totalLoad != NULL)
    {
        for (p = 0; p < Nobjects[POLLUT]; p++)
            stats->totalLoad[p] = OutfallStats[j].totalLoad[p];
    }
    return 0;
}

/* ====================================================================
 *  Link Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getLinkStats(
    int index, swmm_LinkStats *stats)
{
    int k;

    if (LinkStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[LINK])
        return ERR_API_OBJECT_INDEX;

    stats->maxFlow             = LinkStats[index].maxFlow;
    stats->maxFlowDate         = LinkStats[index].maxFlowDate;
    stats->maxVeloc            = LinkStats[index].maxVeloc;
    stats->maxDepth            = LinkStats[index].maxDepth;
    stats->timeNormalFlow      = LinkStats[index].timeNormalFlow;
    stats->timeInletControl    = LinkStats[index].timeInletControl;
    stats->timeSurcharged      = LinkStats[index].timeSurcharged;
    stats->timeFullUpstream    = LinkStats[index].timeFullUpstream;
    stats->timeFullDnstream    = LinkStats[index].timeFullDnstream;
    stats->timeFullFlow        = LinkStats[index].timeFullFlow;
    stats->timeCapacityLimited = LinkStats[index].timeCapacityLimited;
    stats->timeCourantCritical = LinkStats[index].timeCourantCritical;
    stats->flowTurns           = LinkStats[index].flowTurns;
    stats->flowTurnSign        = LinkStats[index].flowTurnSign;

    for (k = 0; k < MAX_FLOW_CLASSES; k++)
        stats->timeInFlowClass[k] = LinkStats[index].timeInFlowClass[k];

    return 0;
}

/* ====================================================================
 *  Pump Statistics
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getPumpStats(
    int index, swmm_PumpStats *stats)
{
    int j;

    if (PumpStats == NULL)
        return ERR_API_NOT_ENDED;
    if (index < 0 || index >= Nobjects[LINK])
        return ERR_API_OBJECT_INDEX;

    /* Find the pump sub-index for this link */
    j = Link[index].subIndex;
    if (Link[index].type != PUMP || j < 0 || j >= Nlinks[PUMP])
        return ERR_API_OBJECT_TYPE;

    stats->utilized     = PumpStats[j].utilized;
    stats->minFlow      = PumpStats[j].minFlow;
    stats->avgFlow      = PumpStats[j].avgFlow;
    stats->maxFlow      = PumpStats[j].maxFlow;
    stats->volume       = PumpStats[j].volume;
    stats->energy       = PumpStats[j].energy;
    stats->offCurveLow  = PumpStats[j].offCurveLow;
    stats->offCurveHigh = PumpStats[j].offCurveHigh;
    stats->startUps     = PumpStats[j].startUps;
    stats->totalPeriods = PumpStats[j].totalPeriods;
    return 0;
}

/* ====================================================================
 *  System Routing Totals (Mass Balance)
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSystemRoutingTotals(
    swmm_RoutingTotals *totals)
{
    totals->dwInflow     = FlowTotals.dwInflow;
    totals->wwInflow     = FlowTotals.wwInflow;
    totals->gwInflow     = FlowTotals.gwInflow;
    totals->iiInflow     = FlowTotals.iiInflow;
    totals->exInflow     = FlowTotals.exInflow;
    totals->flooding     = FlowTotals.flooding;
    totals->outflow      = FlowTotals.outflow;
    totals->evapLoss     = FlowTotals.evapLoss;
    totals->seepLoss     = FlowTotals.seepLoss;
    totals->reacted      = FlowTotals.reacted;
    totals->initStorage  = FlowTotals.initStorage;
    totals->finalStorage = FlowTotals.finalStorage;
    totals->pctError     = FlowTotals.pctError;
    return 0;
}

/* ====================================================================
 *  System Runoff Totals (Mass Balance)
 * ==================================================================== */
int EXPORT_OPENSWMMCORE_SOLVER_API swmm_getSystemRunoffTotals(
    swmm_RunoffTotals *totals)
{
    totals->rainfall       = RunoffTotals.rainfall;
    totals->evap           = RunoffTotals.evap;
    totals->infil          = RunoffTotals.infil;
    totals->runoff         = RunoffTotals.runoff;
    totals->drains         = RunoffTotals.drains;
    totals->runon          = RunoffTotals.runon;
    totals->initStorage    = RunoffTotals.initStorage;
    totals->finalStorage   = RunoffTotals.finalStorage;
    totals->initSnowCover  = RunoffTotals.initSnowCover;
    totals->finalSnowCover = RunoffTotals.finalSnowCover;
    totals->snowRemoved    = RunoffTotals.snowRemoved;
    totals->pctError       = RunoffTotals.pctError;
    return 0;
}
