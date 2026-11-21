//-----------------------------------------------------------------------------
//   routing.c
//
//   Project:  EPA SWMM5
//   Version:  5.2
//   Date:     11/01/21  (Build 5.2.0)
//   Author:   L. Rossman
//             M. Tryby (EPA)
//
//   Conveyance system routing functions.
//
//   Update History
//   ==============
//   Build 5.1.007:
//   - Nodal evap/seepage losses computed using conditions at start of time step.
//   - DWF pollutant concentrations ignored if DWF is negative.
//   - Separate mass balance accounting made for storage evap. & seepage.
//   - Nodal mass balance accounting for negative lateral inflows corrected.
//   Build 5.1.008:
//   - Initialization of flow and quality routing systems moved here from swmm5.c.
//   - Lateral inflows now evaluated at start (not end) of time step.
//   - Flows from LID drains included in lateral inflows.
//   - Conduit evap/seepage losses multiplied by number of barrels before
//     being added into mass balances.
//   Build 5.1.010:
//   - Time when a link's setting is changed is recorded.
//   Build 5.1.011:
//   - Support added for limiting flow routing to specific events.
//   Build 5.1.012:
//   - routing_execute() was re-written so that Routing Events and
//     Skip Steady Flow options work together correctly.
//   Build 5.1.013:
//   - Support added for evaluating controls rules at RuleStep time interval.
//   - Back flow through Outfall nodes now treated as External Inflows for
//     mass balance purposes.
//   - Global infiltration factor for storage seepage set in routing_execute.
//   Build 5.2.0:
//   - Support added for street flow capture and sewer backflow thru inlets.
//   - Shell sort replaces insertion sort for sorting Event array.
//   Build 5.3.0:
//   - Route flows when there is at least one node/link to ensure nodal seepage
//     is applied even with node only models.
//-----------------------------------------------------------------------------

/**
 * @file      routing.c
 * @brief     Coordination of legacy hydraulic routing steps.
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
#include "lid.h"
//-----------------------------------------------------------------------------
// Shared variables
//-----------------------------------------------------------------------------
static int* SortedLinks;
static int NextEvent;
static int BetweenEvents;
static double NewRuleTime;

//-----------------------------------------------------------------------------
//  External functions (declared in funcs.h)
//-----------------------------------------------------------------------------
// routing_open            (called by swmm_start in swmm5.c)
// routing_getRoutingStep  (called by swmm_step in swmm5.c)
// routing_execute         (called by swmm_step in swmm5.c)
// routing_close           (called by swmm_end in swmm5.c)

//-----------------------------------------------------------------------------
// Function declarations
//-----------------------------------------------------------------------------
static int evaluateControlRules(DateTime currentDate, double routingStep);
static void sortEvents(void);
static int isBetweenEvents(DateTime currentDate);
static int isInSteadyState(int actionCount, double stepFlowError);
static int inflowHasChanged(void);

static void initSystemInflows();
static void addSystemInflows(DateTime currentDate, double routingStep);
static void addExternalInflows(DateTime currentDate);
static void addDryWeatherInflows(DateTime currentDate);
static void addWetWeatherInflows(double routingTime);
static void addGroundwaterInflows(double routingTime);
static void addRdiiInflows(DateTime currentDate);
static void addIfaceInflows(DateTime currentDate);
static void addLidDrainInflows(double routingTime);

static int routeFlow(int routingModel, double routingStep);
static void removeSystemOutflows(double routingStep);
static void removeStorageLosses(double tStep);
static void removeConduitLosses(void);
static void removeOutflows(double tStep);

//=============================================================================

/**
 * @brief Initializes the routing analyzer.
 *
 * @return An error code.
 */
int routing_open() {
    // --- open treatment system
    if (!treatmnt_open())
        return ErrorCode;

    // --- topologically sort the links
    SortedLinks = NULL;
    if (Nobjects[LINK] > 0) {
        SortedLinks = (int*)calloc(Nobjects[LINK], sizeof(int));
        if (!SortedLinks) {
            report_writeErrorMsg(ERR_MEMORY, "");
            return ErrorCode;
        }
        toposort_sortLinks(SortedLinks);
        if (ErrorCode)
            return ErrorCode;
    }

    // --- open any routing interface files
    iface_openRoutingFiles();

    // --- initialize flow and quality routing systems
    flowrout_init(RouteModel);
    if (FhotstartInput.mode == NO_FILE)
        qualrout_init();

    // --- initialize routing events
    if (NumEvents > 0)
        sortEvents();
    NextEvent = 0;
    BetweenEvents = (NumEvents > 0);
    NewRuleTime = 0.0;
    return ErrorCode;
}

//=============================================================================

/**
 * @brief Closes down the routing analyzer.
 *
 * @par Implementation arguments
 * - routingModel (in): Routing method code.
 */
void routing_close(int routingModel) {
    // --- close any routing interface files
    iface_closeRoutingFiles();

    // --- free allocated memory
    flowrout_close(routingModel);
    treatmnt_close();
    FREE(SortedLinks);
}

/*!
* \brief Determines time step used for flow routing at current time period.
* @par Implementation arguments
 * - routingModel (in): Routing method code
* - fixedStep (in): User-supplied time step (sec)
* \return Time step used for flow routing (sec)
 *

 */
double routing_getRoutingStep(int routingModel, double fixedStep) {
    double date1, date2, nextTime;
    double routingStep = 0.0, nextRuleTime, nextRoutingTime;

    if (Nobjects[LINK] == 0)
        return fixedStep;

    // --- find largest step possible if between routing events
    if (NumEvents > 0 && BetweenEvents) {
        nextTime = MIN(NewRunoffTime, ReportTime);
        date1 = getDateTime(NewRoutingTime);
        date2 = getDateTime(nextTime);
        if (date2 > date1 && date2 < Event[NextEvent].start) {
            routingStep = (nextTime - NewRoutingTime) / 1000.0;
        } else {
            date1 = getDateTime(NewRoutingTime + 1000.0 * fixedStep);
            if (date1 < Event[NextEvent].start)
                return fixedStep;
        }
    }

    // --- otherwise use a regular flow-routing based time step
    if (routingStep == 0.0) {
        routingStep = flowrout_getRoutingStep(routingModel, fixedStep);
    }

    // --- determine if control rule time interval reached
    if (RuleStep > 0) {
        nextRuleTime = NewRuleTime + 1000. * RuleStep;
        nextRoutingTime = NewRoutingTime + 1000. * routingStep;
        if (nextRoutingTime >= nextRuleTime) {
            routingStep = (nextRuleTime - NewRoutingTime) / 1000.0;
        }
    }
    return routingStep;
}

//=============================================================================

/**
 * @brief Executes the routing process at the current time period.
 *
 * @par Implementation arguments
 * - routingModel (in): Routing method code.
 * - routingStep (in): Routing time step (sec).
 */
void routing_execute(int routingModel, double routingStep) {
    int trialsCount = 1;      // trials required to solve flow routing
    int actionCount = 0;      // number of control actions taken
    int inSteadyState = TRUE; // system is in steady state
    DateTime currentDate;     // date at start of routing step
    double stepFlowError;     // 1 - (system outflow) / (system inflow)

    if (ErrorCode)
        return;

    // --- update mass balance totals over previous half time step
    massbal_updateRoutingTotals(routingStep / 2.);

    // --- take any applicable control rule actions
    currentDate = getDateTime(NewRoutingTime);
    actionCount = evaluateControlRules(currentDate, routingStep);

    // --- initialize mass balance and system inflow variables
    stepFlowError = massbal_getStepFlowError();
    massbal_initTimeStepTotals();
    initSystemInflows();

    // --- check that current date falls within a user-speficied event period
    BetweenEvents = isBetweenEvents(currentDate);
    if (BetweenEvents == FALSE) {
        // --- apply current inflows to conveyance system
        addSystemInflows(currentDate, routingStep);
        inlet_findCapturedFlows(routingStep);

        // --- route flows if system is not in steady state
        inSteadyState = isInSteadyState(actionCount, stepFlowError);
        if (inSteadyState == FALSE)
            trialsCount = routeFlow(routingModel, routingStep);

        // --- route water quality constituents
        if (Nobjects[POLLUT] > 0 && !IgnoreQuality) {
            inlet_adjustQualInflows();
            qualrout_execute(routingStep);
        }

        // --- update mass balance totals for flows leaving the system
        removeSystemOutflows(routingStep);
        inlet_adjustQualOutflows();

        // --- update time step & flow routing statistics
        if (Nobjects[LINK] > 0) {
            stats_updateFlowStats(routingStep, getDateTime(NewRoutingTime));
            stats_updateTimeStepStats(routingStep, trialsCount, inSteadyState);
        }
    }

    // --- update mass balance totals over the current half time step
    massbal_updateRoutingTotals(routingStep / 2.);

    // --- A3 parity tracing (env-gated, zero cost when SWMM_TRACE_RSTEP
    //     unset): one CSV row per routing step, format-matched to the
    //     refactored trace in SWMMEngine::stepRouting for first-divergence
    //     hunting between the engines.
    {
        static FILE* traceFile = NULL;
        static int traceInit = FALSE;
        if (!traceInit) {
            char* p = getenv("SWMM_TRACE_RSTEP");
            traceInit = TRUE;
            if (p && *p) {
                traceFile = fopen(p, "w");
                if (traceFile)
                    fprintf(traceFile,
                            "step,new_ms,dt_ms,iters,qsum,ysum,lsum,rosum,qhash,yhash\n");
            }
        }
        if (traceFile) {
            static long traceSn = 0;
            extern long SwmmTraceRstepSn; /* defined in dwflow.c */
            int tj;
            double qSum = 0.0, ySum = 0.0, lSum = 0.0, roSum = 0.0;
            /* FNV-1a 64-bit hashes over the raw bit patterns of link flow &
               node depth (element order) — exact first-divergence detector
               (the %a sums absorb small-magnitude element diffs). */
            unsigned long long qHash = 14695981039346656037ULL;
            unsigned long long yHash = 14695981039346656037ULL;
            unsigned long long bits;
            for (tj = 0; tj < Nobjects[LINK]; tj++) {
                qSum += Link[tj].newFlow;
                memcpy(&bits, &Link[tj].newFlow, sizeof bits);
                qHash = (qHash ^ bits) * 1099511628211ULL;
            }
            for (tj = 0; tj < Nobjects[NODE]; tj++) {
                ySum += Node[tj].newDepth;
                memcpy(&bits, &Node[tj].newDepth, sizeof bits);
                yHash = (yHash ^ bits) * 1099511628211ULL;
            }
            for (tj = 0; tj < Nobjects[NODE]; tj++)
                lSum += Node[tj].newLatFlow;
            for (tj = 0; tj < Nobjects[SUBCATCH]; tj++)
                roSum += Subcatch[tj].newRunoff;
            fprintf(traceFile, "%ld,%.6f,%.6f,%d,%a,%a,%a,%a,%016llx,%016llx\n", ++traceSn,
                    NewRoutingTime, 1000.0 * routingStep, trialsCount, qSum, ySum, lSum, roSum,
                    qHash, yHash);
            SwmmTraceRstepSn = traceSn; /* step-gate for dwflow link trace */

            // Optional per-element dump at one step (SWMM_TRACE_DUMP_STEP=N):
            // writes "<trace>.dumpN" with per-link newFlow/dqdh and per-node
            // newDepth/inflow/outflow in %a for element-level first-divergence
            // pinpointing against the refactored engine.
            {
                static long dumpStep = -1;
                static int dumpInit = FALSE;
                if (!dumpInit) {
                    char* d = getenv("SWMM_TRACE_DUMP_STEP");
                    dumpInit = TRUE;
                    if (d && *d)
                        dumpStep = atol(d);
                }
                if (traceSn == dumpStep) {
                    char fname[512];
                    FILE* df;
                    snprintf(fname, sizeof(fname), "%s.dump%ld", getenv("SWMM_TRACE_RSTEP"),
                             dumpStep);
                    df = fopen(fname, "w");
                    if (df) {
                        for (tj = 0; tj < Nobjects[LINK]; tj++)
                            fprintf(df, "L,%d,%a,%a\n", tj, Link[tj].newFlow, Link[tj].dqdh);
                        for (tj = 0; tj < Nobjects[NODE]; tj++)
                            fprintf(df, "N,%d,%a,%a,%a,%a,%a\n", tj, Node[tj].newDepth,
                                    Node[tj].inflow, Node[tj].outflow, Node[tj].newLatFlow,
                                    Node[tj].oldLatFlow);
                        for (tj = 0; tj < Nobjects[SUBCATCH]; tj++)
                            fprintf(df, "S,%d,%a,%a,%a,%a\n", tj, Subcatch[tj].newRunoff,
                                    Subcatch[tj].rainfall, Subcatch[tj].infilLoss,
                                    Subcatch[tj].oldRunoff);
                        fclose(df);
                    }
                }
            }
        }
    }
}

//=============================================================================

/**
 * @brief Apply due control actions and advance routing/control clocks.
 * @param[in] currentDate Absolute SWMM calendar date/time in days.
 * @param[in] routingStep Positive hydraulic step in seconds; converted to
 * days for rule evaluation and milliseconds for shared routing clocks.
 * @return Count of links whose actual setting differed from the target.
 * @post Updates target/actual link settings, timeLastSet, OldRoutingTime,
 * NewRoutingTime and, when reached, NewRuleTime. Rule evaluation is skipped
 * until its scheduled time unless RuleStep is zero.
 */
int evaluateControlRules(DateTime currentDate, double routingStep) {
    int j;
    int actionCount = 0;

    // --- find new link target settings that are not related to
    // --- control rules (e.g., pump on/off depth limits)
    for (j = 0; j < Nobjects[LINK]; j++)
        link_setTargetSetting(j);

    // --- evaluate control rules if next evaluation time reached
    if (RuleStep == 0 || fabs(NewRoutingTime - NewRuleTime) < 1.0) {
        controls_evaluate(currentDate, currentDate - StartDateTime, routingStep / SECperDAY);
    }

    // --- change each link's actual setting if it differs from its target
    for (j = 0; j < Nobjects[LINK]; j++) {
        if (Link[j].targetSetting != Link[j].setting) {
            // --- update time when link was switched between open & closed
            if (Link[j].targetSetting * Link[j].setting == 0.0)
                Link[j].timeLastSet = currentDate;

            // --- implement the change in the link's setting
            link_setSetting(j, routingStep);
            actionCount++;
        }
    }

    // --- update value of elapsed routing time (in milliseconds)
    OldRoutingTime = NewRoutingTime;
    NewRoutingTime = NewRoutingTime + 1000.0 * routingStep;

    // --- see if control rule evaluation time should be advanced
    if (fabs(NewRoutingTime - (NewRuleTime + 1000.0 * RuleStep)) < 1)
        NewRuleTime += 1000.0 * RuleStep;
    return actionCount;
}

//=============================================================================

/**
 * @brief Prepare old quality state and clear new lateral inflows for a routing step.
 * @post Copies current node/link quality to old state, selects the global
 * infiltration factor for storage seepage, moves node newLatFlow to oldLatFlow
 * and clears newLatFlow. Reads the initialized global model.
 */
void initSystemInflows() {
    int j;

    // --- replace old water quality state with new state
    if (Nobjects[POLLUT] > 0) {
        for (j = 0; j < Nobjects[NODE]; j++)
            node_setOldQualState(j);
        for (j = 0; j < Nobjects[LINK]; j++)
            link_setOldQualState(j);
    }

    // --- set infiltration factor for storage unit seepage
    //     (-1 argument indicates global factor is used)
    infil_setInfilFactor(-1);

    // --- initialize lateral inflows at nodes
    for (j = 0; j < Nobjects[NODE]; j++) {
        Node[j].oldLatFlow = Node[j].newLatFlow;
        Node[j].newLatFlow = 0.0;
    }
}

//=============================================================================

/**
 * @brief Test whether a date lies outside the next configured simulation event.
 * @param[in] currentDate Absolute SWMM calendar date/time in days.
 * @return TRUE before an event starts or after it ends; FALSE within an
 * event or when no events are configured. Event endpoints are inclusive.
 * @post Increments NextEvent when the current event has ended, at most once per call.
 * @pre When NumEvents > 0, Event[NextEvent] is a valid allocated entry;
 * caller event scheduling must prevent reading beyond the final event.
 */
int isBetweenEvents(DateTime currentDate) {
    // --- if no events defined then result is always false
    if (NumEvents == 0)
        return FALSE;

    // --- currrent event period has ended so result is true
    if (currentDate > Event[NextEvent].end) {
        NextEvent++;
        return TRUE;
    }

    // --- we've entered the next event period so result is false
    else if (currentDate >= Event[NextEvent].start) {
        return FALSE;
    }
    return TRUE;
}

//=============================================================================

/**
 * @brief Accumulate every configured lateral inflow source and node water loss.
 * @param[in] currentDate Absolute SWMM calendar date/time in days, used for
 * external, dry-weather, RDII and interface forcing lookups.
 * @param[in] routingStep Positive interval in seconds for storage-node losses.
 * @post Updates Node.losses/newLatFlow/qualInflow, quality loads and source
 * mass balances. Runoff/groundwater/LID sources are sampled at OldRoutingTime
 * in milliseconds. Requires initialized inflow accumulators for this step.
 */
void addSystemInflows(DateTime currentDate, double routingStep) {
    int j;

    // --- find evap. & seepage losses from storage nodes
    for (j = 0; j < Nobjects[NODE]; j++)
        Node[j].losses = node_getLosses(j, routingStep);

    // --- add lateral inflows at nodes
    addExternalInflows(currentDate);
    addDryWeatherInflows(currentDate);
    addWetWeatherInflows(OldRoutingTime);
    addGroundwaterInflows(OldRoutingTime);
    addLidDrainInflows(OldRoutingTime);
    addRdiiInflows(currentDate);
    addIfaceInflows(currentDate);

    // --- initialize node inflow for quality routing
    for (j = 0; j < Nobjects[NODE]; j++)
        Node[j].qualInflow = MAX(0.0, Node[j].newLatFlow);
}

//=============================================================================

/**
 * @brief Decide whether steady-state hydraulic routing may be skipped.
 * @param[in] actionCount Number of control/setting actions in the current step.
 * @param[in] stepFlowError Dimensionless current flow imbalance, compared
 * in the same scale as SysFlowTol.
 * @return TRUE only when SkipSteadyState is enabled, routing has advanced,
 * no actions occurred, abs(stepFlowError) <= SysFlowTol and inflows are unchanged.
 */
int isInSteadyState(int actionCount, double stepFlowError) {
    // --- check if can skip steady state periods based on flows
    if (SkipSteadyState) {
        if (OldRoutingTime == 0.0 || actionCount > 0 || fabs(stepFlowError) > SysFlowTol ||
            inflowHasChanged())
            return FALSE;
        else
            return TRUE;
    }
    return FALSE;
}

//=============================================================================

/**
 * @brief Shift hydraulic states, initialize node flows and execute network routing.
 * @param[in] routingModel Hydraulic routing-method code passed to flowrout_execute().
 * @param[in] routingStep Positive routing interval in seconds.
 * @return flowrout_execute()'s step count, or one when no nodes/links exist.
 * @post Moves current hydraulic states to old states, initializes node
 * flow/loss/overflow terms and advances network hydraulics in sorted-link order.
 * @pre SortedLinks and method-specific routing workspace are initialized.
 */
int routeFlow(int routingModel, double routingStep) {
    int j;
    int stepCount = 1;

    // --- replace old hydraulic state values with current ones
    for (j = 0; j < Nobjects[LINK]; j++)
        link_setOldHydState(j);
    for (j = 0; j < Nobjects[NODE]; j++)
        node_setOldHydState(j);

    // --- initialize node inflows to lateral flows, outflows to evap +
    //     seepage losses, & overflows to excess stored volume
    for (j = 0; j < Nobjects[NODE]; j++)
        node_initFlows(j, routingStep);

    // --- route flow through the drainage network if there is at least
    //     one link or node
    if (Nobjects[LINK] > 0 || Nobjects[NODE] > 0) {
        stepCount = flowrout_execute(SortedLinks, routingModel, routingStep);
    }

    // --- save overflows at inlet capture nodes as inlet backflow
    return stepCount;
}

//=============================================================================

/**
 * @brief Apply storage/conduit water losses and account for system boundary outflows.
 * @param[in] routingStep Positive routing interval in seconds.
 * @post Updates stored water through removeStorageLosses(), accounts for
 * conduit evaporation/seepage, then adds boundary/flooding flows and pollutant
 * loads to shared mass balances. Requires current routed hydraulic/quality state.
 */
void removeSystemOutflows(double routingStep) {
    // --- remove evaporation, infiltration & outflows from system
    removeStorageLosses(routingStep);
    removeConduitLosses();
    removeOutflows(routingStep);
}

//=============================================================================

/**
 * @brief Adds direct external inflows to nodes at current date.
 *
 * @param[in] currentDate Current date/time.
 */
void addExternalInflows(DateTime currentDate) {
    int j, p;
    double q, w;
    TExtInflow* inflow;

    // --- for each node with a defined external inflow
    for (j = 0; j < Nobjects[NODE]; j++) {
        // --- get flow inflow
        q = Node[j].apiExtInflow;
        inflow = Node[j].extInflow;
        while (inflow) {
            if (inflow->type == FLOW_INFLOW) {
                q += inflow_getExtInflow(inflow, currentDate);
                break;
            } else
                inflow = inflow->next;
        }
        if (fabs(q) < FLOW_TOL)
            q = 0.0;

        // --- add flow inflow to node's lateral inflow
        Node[j].newLatFlow += q;
        if (q >= 0.0)
            massbal_addInflowFlow(EXTERNAL_INFLOW, q);
        else {
            massbal_addOutflowFlow(-q, FALSE);
            continue;
        }

        // --- add on any inflow (i.e., reverse flow) through an outfall
        if (Node[j].type == OUTFALL && Node[j].oldNetInflow < 0.0) {
            q = q - Node[j].oldNetInflow;
        }

        // --- get pollutant mass inflows
        inflow = Node[j].extInflow;
        while (inflow) {
            if (inflow->type != FLOW_INFLOW) {
                p = inflow->param;
                w = inflow_getExtInflow(inflow, currentDate);
                if (inflow->type == CONCEN_INFLOW)
                    w *= q;
                Node[j].newQual[p] += w;
                massbal_addInflowQual(EXTERNAL_INFLOW, p, w);
            }
            inflow = inflow->next;
        }

        // --- add api mass fluxes to node's inflow
        for (p = 0; p < Nobjects[POLLUT]; p++) {
            w = Node[j].apiExtQualMassFlux[p];

            if (w > 0.0) {
                Node[j].newQual[p] += w;
                massbal_addInflowQual(EXTERNAL_INFLOW, p, w);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Adds dry weather inflows to nodes at current date.
 *
 * @param[in] currentDate Current date/time.
 */
void addDryWeatherInflows(DateTime currentDate) {
    int j, p;
    int month, day, hour;
    double q, w;
    TDwfInflow* inflow;

    // --- get month (zero-based), day-of-week (zero-based),
    //     & hour-of-day for routing date/time
    month = datetime_monthOfYear(currentDate) - 1;
    day = datetime_dayOfWeek(currentDate) - 1;
    hour = datetime_hourOfDay(currentDate);

    // --- for each node with a defined dry weather inflow
    for (j = 0; j < Nobjects[NODE]; j++) {
        inflow = Node[j].dwfInflow;
        if (!inflow)
            continue;

        // --- get flow inflow (i.e., the inflow whose param code is -1)
        q = 0.0;
        while (inflow) {
            if (inflow->param < 0) {
                q = inflow_getDwfInflow(inflow, month, day, hour);
                break;
            }
            inflow = inflow->next;
        }
        if (fabs(q) < FLOW_TOL)
            q = 0.0;

        // --- add flow inflow to node's lateral inflow
        Node[j].newLatFlow += q;
        massbal_addInflowFlow(DRY_WEATHER_INFLOW, q);

        // --- stop if inflow is non-positive
        if (q <= 0.0)
            continue;

        // --- add default DWF pollutant inflows
        for (p = 0; p < Nobjects[POLLUT]; p++) {
            if (Pollut[p].dwfConcen > 0.0) {
                w = q * Pollut[p].dwfConcen;
                Node[j].newQual[p] += w;
                massbal_addInflowQual(DRY_WEATHER_INFLOW, p, w);
            }
        }

        // --- get pollutant mass inflows
        inflow = Node[j].dwfInflow;
        while (inflow) {
            if (inflow->param >= 0) {
                p = inflow->param;
                w = q * inflow_getDwfInflow(inflow, month, day, hour);
                Node[j].newQual[p] += w;
                massbal_addInflowQual(DRY_WEATHER_INFLOW, p, w);

                // --- subtract off any default inflow
                if (Pollut[p].dwfConcen > 0.0) {
                    w = q * Pollut[p].dwfConcen;
                    Node[j].newQual[p] -= w;
                    massbal_addInflowQual(DRY_WEATHER_INFLOW, p, -w);
                }
            }
            inflow = inflow->next;
        }
    }
}

//=============================================================================

/**
 * @brief Adds runoff inflows to nodes at current elapsed time.
 *
 * @param[in] routingTime Elasped time (millisec).
 */
void addWetWeatherInflows(double routingTime) {
    int i, j, p;
    double q, w;
    double f;

    // --- find where current routing time lies between latest runoff times
    if (Nobjects[SUBCATCH] == 0)
        return;
    f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime);
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;

    // for each subcatchment outlet node,
    // add interpolated runoff flow & pollutant load to node's inflow
    for (i = 0; i < Nobjects[SUBCATCH]; i++) {
        j = Subcatch[i].outNode;
        if (j >= 0) {
            // add runoff flow to lateral inflow
            q = subcatch_getWtdOutflow(i, f); // current runoff flow
            Node[j].newLatFlow += q;
            massbal_addInflowFlow(WET_WEATHER_INFLOW, q);

            // add pollutant load
            for (p = 0; p < Nobjects[POLLUT]; p++) {
                w = surfqual_getWtdWashoff(i, p, f);
                Node[j].newQual[p] += w;
                massbal_addInflowQual(WET_WEATHER_INFLOW, p, w);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Adds groundwater inflows to nodes at current elapsed time.
 *
 * @param[in] routingTime Elasped time (millisec).
 */
void addGroundwaterInflows(double routingTime) {
    int i, j, p;
    double q, w;
    double f;
    TGroundwater* gw;

    // --- find where current routing time lies between latest runoff times
    if (Nobjects[SUBCATCH] == 0)
        return;
    f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime);
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;

    // --- for each subcatchment
    for (i = 0; i < Nobjects[SUBCATCH]; i++) {
        // --- see if subcatch contains groundwater
        gw = Subcatch[i].groundwater;
        if (gw) {
            // --- identify node receiving groundwater flow
            j = gw->node;
            if (j >= 0) {
                // add groundwater flow to lateral inflow
                q = ((1.0 - f) * (gw->oldFlow) + f * (gw->newFlow)) * Subcatch[i].area;
                if (fabs(q) < FLOW_TOL)
                    continue;
                Node[j].newLatFlow += q;
                massbal_addInflowFlow(GROUNDWATER_INFLOW, q);

                // add pollutant load (for positive inflow)
                if (q > 0.0) {
                    for (p = 0; p < Nobjects[POLLUT]; p++) {
                        w = q * Pollut[p].gwConcen;
                        Node[j].newQual[p] += w;
                        massbal_addInflowQual(GROUNDWATER_INFLOW, p, w);
                    }
                }
            }
        }
    }
}

//=============================================================================

/**
 * @brief Adds inflows to nodes receiving LID drain flow.
 *
 * @param[in] routingTime Elasped time (millisec).
 */
void addLidDrainInflows(double routingTime) {
    int j;
    double f;

    // for each subcatchment
    if (Nobjects[SUBCATCH] == 0)
        return;
    f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime);
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    for (j = 0; j < Nobjects[SUBCATCH]; j++) {
        if (Subcatch[j].area > 0.0 && Subcatch[j].lidArea > 0.0)
            lid_addDrainInflow(j, f);
    }
}

//=============================================================================

/**
 * @brief Adds RDII inflows to nodes at current date.
 *
 * @param[in] currentDate Current date/time.
 */
void addRdiiInflows(DateTime currentDate) {
    int i, j, p;
    double q, w;
    int numRdiiNodes;

    // --- see if any nodes have RDII at current date
    numRdiiNodes = rdii_getNumRdiiFlows(currentDate);

    // --- add RDII flow to each node's lateral inflow
    for (i = 0; i < numRdiiNodes; i++) {
        rdii_getRdiiFlow(i, &j, &q);
        if (j < 0)
            continue;
        if (fabs(q) < FLOW_TOL)
            continue;
        Node[j].newLatFlow += q;
        massbal_addInflowFlow(RDII_INFLOW, q);

        // add pollutant load (for positive inflow)
        if (q > 0.0) {
            for (p = 0; p < Nobjects[POLLUT]; p++) {
                w = q * Pollut[p].rdiiConcen;
                Node[j].newQual[p] += w;
                massbal_addInflowQual(RDII_INFLOW, p, w);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Adds inflows from routing interface file to nodes at current date.
 *
 * @param[in] currentDate Current date/time.
 */
void addIfaceInflows(DateTime currentDate) {
    int i, j, p;
    double q, w;
    int numIfaceNodes;

    // --- see if any nodes have interface inflows at current date
    if (Finflows.mode != USE_FILE)
        return;
    numIfaceNodes = iface_getNumIfaceNodes(currentDate);

    // --- add interface flow to each node's lateral inflow
    for (i = 0; i < numIfaceNodes; i++) {
        j = iface_getIfaceNode(i);
        if (j < 0)
            continue;
        q = iface_getIfaceFlow(i);
        if (fabs(q) < FLOW_TOL)
            continue;
        Node[j].newLatFlow += q;
        massbal_addInflowFlow(EXTERNAL_INFLOW, q);

        // add pollutant load (for positive inflow)
        if (q > 0.0) {
            for (p = 0; p < Nobjects[POLLUT]; p++) {
                w = q * iface_getIfaceQual(i, p);
                Node[j].newQual[p] += w;
                massbal_addInflowQual(EXTERNAL_INFLOW, p, w);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Checks if the hydraulic state of the system has changed from the previous time step.
 *
 * @return TRUE if external inflows or outfall flows have changed from the previous time step.
 */
int inflowHasChanged() {
    int j;
    double diff, qOld, qNew;

    // --- check if external inflows or outfall flows have changed
    for (j = 0; j < Nobjects[NODE]; j++) {
        qOld = Node[j].oldLatFlow;
        qNew = Node[j].newLatFlow;
        if (fabs(qOld) > TINY)
            diff = (qNew / qOld) - 1.0;
        else if (fabs(qNew) > TINY)
            diff = 1.0;
        else
            diff = 0.0;
        if (fabs(diff) > LatFlowTol)
            return TRUE;
        if (Node[j].type == OUTFALL || Node[j].degree == 0) {
            qOld = Node[j].oldFlowInflow;
            qNew = Node[j].inflow;
            if (fabs(qOld) > TINY)
                diff = (qNew / qOld) - 1.0;
            else if (fabs(qNew) > TINY)
                diff = 1.0;
            else
                diff = 0.0;
            if (fabs(diff) > LatFlowTol)
                return TRUE;
        }
    }
    return FALSE;
}

//=============================================================================

/**
 * @brief Adds flow rate lost from all storage nodes due to evaporation & seepage in current time
 *        step to overall mass balance totals.
 *
 * @param[in] tStep Routing time step (sec).
 */
void removeStorageLosses(double tStep) {
    int i;
    double evapLoss = 0.0, exfilLoss = 0.0;

    // --- check each storage node
    for (i = 0; i < Nobjects[NODE]; i++) {
        if (Node[i].type == STORAGE) {
            // --- update total system storage losses
            evapLoss += Storage[Node[i].subIndex].evapLoss;
            exfilLoss += Storage[Node[i].subIndex].exfilLoss;
        }
    }

    // --- add loss rates (ft3/sec) to time step's mass balance
    massbal_addNodeLosses(evapLoss / tStep, exfilLoss / tStep);
}

//=============================================================================

/**
 * @brief Adds flow rate lost from all conduits due to evaporation & seepage over current time step
 *        to overall mass balance.
 */
void removeConduitLosses() {
    int i, k;
    double barrels, evapLoss = 0.0, seepLoss = 0.0;

    for (i = 0; i < Nobjects[LINK]; i++) {
        if (Link[i].type == CONDUIT) {
            // --- retrieve number of barrels
            k = Link[i].subIndex;
            barrels = Conduit[k].barrels;

            // --- update total conduit losses
            evapLoss += Conduit[k].evapLossRate * barrels;
            seepLoss += Conduit[k].seepLossRate * barrels;
        }
    }
    massbal_addLinkLosses(evapLoss, seepLoss);
}

//=============================================================================

/**
 * @brief Account for system outflows, flooding and negative lateral pollutant loads.
 * @param[in] tStep Routing interval in seconds, used to accumulate routed
 * outfall volumes and pollutant loads for receiving subcatchments.
 * @post Updates outfall vRouted/wRouted and mass-balance inflow/outflow rates
 * using current node flows and quality. Does not directly remove stored node volume.
 * @pre Current node hydraulic and quality results are available for the step.
 */
void removeOutflows(double tStep)
//
//  Input:   none
//  Output:  none
//  Purpose: finds flows that leave the system and adds these to mass
//           balance totals.
//
{
    int i, p, k;
    int isFlooded;
    double q, w, v;

    for (i = 0; i < Nobjects[NODE]; i++) {
        // --- accumulate inflow volume & pollut. load at outfalls
        if (Node[i].type == OUTFALL && Node[i].inflow > 0.0) {
            k = Node[i].subIndex;
            if (Outfall[k].routeTo >= 0) {
                v = Node[i].inflow * tStep;
                Outfall[k].vRouted += v;
                for (p = 0; p < Nobjects[POLLUT]; p++)
                    Outfall[k].wRouted[p] += Node[i].newQual[p] * v;
            }
        }

        // --- update mass balance with flow and mass leaving the system
        //     through outfalls and flooded interior nodes
        q = node_getSystemOutflow(i, &isFlooded);
        if (q > 0.0) {
            massbal_addOutflowFlow(q, isFlooded);
            for (p = 0; p < Nobjects[POLLUT]; p++) {
                w = q * Node[i].newQual[p];
                massbal_addOutflowQual(p, w, isFlooded);
            }
        } else
            massbal_addInflowFlow(EXTERNAL_INFLOW, -q);

        // --- update mass balance with mass leaving system through negative
        //     lateral inflows (lateral flow was previously accounted for)
        q = Node[i].newLatFlow;
        if (q < 0.0) {
            for (p = 0; p < Nobjects[POLLUT]; p++) {
                w = -q * Node[i].newQual[p];
                massbal_addOutflowQual(p, w, FALSE);
            }
        }
    }
}

//=============================================================================

/**
 * @brief Sorts the entries of the Event array in chronological order.
 */
void sortEvents() {
    int i, j, gap;
    TEvent temp;

    // Apply shell sort to event list
    for (gap = NumEvents / 2; gap >= 1; gap /= 2) {
        for (i = gap; i < NumEvents; i += gap) {
            temp = Event[i];
            j = i - gap;
            while (j >= 0 && Event[j].start > temp.start) {
                Event[j + gap] = Event[j];
                j -= gap;
            }
            if (j != i - gap)
                Event[j + gap] = temp;
        }
    }

    // Adjust for overlapping events
    for (i = 0; i < NumEvents - 1; i++) {
        if (Event[i].end > Event[i + 1].start)
            Event[i].end = Event[i + 1].start;
    }
}

//=============================================================================
