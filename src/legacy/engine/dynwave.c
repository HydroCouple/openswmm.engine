//-----------------------------------------------------------------------------
//   dynwave.c
//
//   Project:  EPA SWMM5
//   Version:  5.2
//   Date:     07/13/23  (Build 5.2.4)
//   Author:   L. Rossman
//             M. Tryby (EPA)
//             R. Dickinson (CDM)
//
//   Dynamic wave flow routing functions.
//
//   This module solves the dynamic wave flow routing equations using
//   Picard Iterations (i.e., a method of successive approximations)
//   to solve the explicit form of the continuity and momentum equations
//   for conduits.
//
//   Update History
//   ==============
//   Build 5.1.002:
//   - Only non-ponded nodal surface area is saved for use in
//     surcharge algorithm.
//   Build 5.1.007:
//   - Node losses added to node outflow variable instead of treated
//     as a separate item when computing change in node flow volume.
//   Build 5.1.008:
//   - Module-specific constants moved here from project.c.
//   - Support added for user-specified minimum variable time step.
//   - Node crown elevations found here instead of in flowrout.c module.
//   - OpenMP use to parallelize findLinkFlows() & findNodeDepths().
//   - Bug in finding complete list of capacity limited links fixed.
//   Build 5.1.011:
//   - Added test for failed memory allocation.
//   - Fixed illegal array index bug for Ideal Pumps.
//   Build 5.1.013:
//   - Include omp.h protected against lack of compiler support for OpenMP.
//   - SurchargeMethod option used to decide how node surcharging is handled.
//   - Storage nodes allowed to pressurize if their surcharge depth > 0.
//   - Minimum flow needed to compute a Courant time step modified.
//   Build 5.1.014:
//   - updateNodeFlows() modified to subtract conduit evap. and seepage losses
//     from downstream node inflow instead of upstream node outflow.
//   Build 5.1.015:
//   - Roll back the 5.1.014 change for conduit losses in updateNodeFlows().
//   Build 5.2.0:
//   - Support added for reporting most frequent non-converging links.
//   Build 5.2.4:
//   - Conduit evap+seepage outflow split evenly between outflow from
//     conduit's upstream and non-outfall downstream nodes.
//   Build 5.3.0:
//   - Enable drainage in isolated storage with surcharge enabled.
//-----------------------------------------------------------------------------

/**
 * @file      dynwave.c
 * @brief     Dynamic-wave network routing and iteration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <stdlib.h>
#include <math.h>
#include "headers.h"

//-----------------------------------------------------------------------------
//     Constants
//-----------------------------------------------------------------------------
static const double MINTIMESTEP = 0.001;          // min. time step (sec)
static const double OMEGA = 0.5;                  // under-relaxation parameter
static const double DEFAULT_SURFAREA = 12.566;    // Min. nodal surface area (~4 ft diam.)
static const double DEFAULT_HEADTOL = 0.005;      // Default head tolerance (ft)
static const double EXTRAN_CROWN_CUTOFF = 0.96;   // crown cutoff for EXTRAN
static const double SLOT_CROWN_CUTOFF = 0.985257; // crown cutoff for SLOT
static const int DEFAULT_MAXTRIALS = 8;           // Max. trials per time step

//-----------------------------------------------------------------------------
//  Data Structures
//-----------------------------------------------------------------------------
typedef struct {
    char converged;     // TRUE if iterations for a node done
    double newSurfArea; // current surface area (ft2)
    double oldSurfArea; // previous surface area (ft2)
    double sumdqdh;     // sum of dqdh from adjoining links
    double dYdT;        // change in depth w.r.t. time (ft/sec)
    double conduitLoss; // aggregate conduit loss charged to this node (cfs)
    double lossFactor;  // fraction affordable without drying below zero
    double conduitVolumeChange; // assigned change in adjacent conduit storage (ft3)
} TXnode;

//-----------------------------------------------------------------------------
//  Shared Variables
//-----------------------------------------------------------------------------
static double VariableStep; // size of variable time step (sec)
static TXnode* Xnode;       // extended nodal information

static int HasConduitLosses; // evaporation or seepage possible this step
static double Omega; // actual under-relaxation parameter
static int Steps;    // number of Picard iterations

//-----------------------------------------------------------------------------
//  Function declarations
//-----------------------------------------------------------------------------
static void initRoutingStep(void);
static void initNodeStates(void);
static void findBypassedLinks();
static void findLimitedLinks();

static void findLinkFlows(double dt);
static int isTrueConduit(int link);
static void findNonConduitFlow(int link, double dt);
static void findNonConduitSurfArea(int link);
static double getModPumpFlow(int link, double q, double dt);
static void updateNodeFlows(int link);
static void limitConduitLosses(double dt);
static void updateConvergenceStats();

static int findNodeDepths(double dt);
static void setNodeDepth(int node, double dt);
static double getFloodedDepth(int node, int canPond, double dV, double yNew, double yMax,
                              double dt);

static double getVariableStep(double maxStep);
static double getLinkStep(double tMin, int* minLink);
static double getNodeStep(double tMin, int* minNode);

/*!
* \brief Initializes dynamic wave routing method.
*/
void dynwave_init() {
    int i, j;
    double z;

    VariableStep = 0.0;
    Xnode = (TXnode*)calloc(Nobjects[NODE], sizeof(TXnode));
    if (Xnode == NULL) {
        report_writeErrorMsg(ERR_MEMORY, " Not enough memory for dynamic wave routing.");
        return;
    }

    // --- initialize node surface areas & crown elev.
    for (i = 0; i < Nobjects[NODE]; i++) {
        Xnode[i].newSurfArea = 0.0;
        Xnode[i].oldSurfArea = 0.0;
        Node[i].crownElev = Node[i].invertElev;
    }

    // --- initialize links & update node crown elevations
    for (i = 0; i < Nobjects[LINK]; i++) {
        j = Link[i].node1;
        z = Node[j].invertElev + Link[i].offset1 + Link[i].xsect.yFull;
        Node[j].crownElev = MAX(Node[j].crownElev, z);

        j = Link[i].node2;
        z = Node[j].invertElev + Link[i].offset2 + Link[i].xsect.yFull;
        Node[j].crownElev = MAX(Node[j].crownElev, z);
        Link[i].flowClass = DRY;
        Link[i].dqdh = 0.0;
    }

    // --- set crown cutoff for finding top width of closed conduits
    if (SurchargeMethod == SLOT)
        CrownCutoff = SLOT_CROWN_CUTOFF;
    else
        CrownCutoff = EXTRAN_CROWN_CUTOFF;
}

//=============================================================================

/**
 * @brief Frees memory allocated for dynamic wave routing method.
 */
void dynwave_close() {
    FREE(Xnode);
}

/*!
* \brief Adjusts dynamic wave routing options.
*/
void dynwave_validate() {
    if (MinRouteStep > RouteStep)
        MinRouteStep = RouteStep;
    if (MinRouteStep < MINTIMESTEP)
        MinRouteStep = MINTIMESTEP;
    if (MinSurfArea == 0.0)
        MinSurfArea = DEFAULT_SURFAREA;
    else
        MinSurfArea /= UCF(LENGTH) * UCF(LENGTH);
    if (HeadTol == 0.0)
        HeadTol = DEFAULT_HEADTOL;
    else
        HeadTol /= UCF(LENGTH);
    if (MaxTrials == 0)
        MaxTrials = DEFAULT_MAXTRIALS;
}

/*!
* \brief Computes variable routing time step if applicable.
* @par Implementation arguments
 * - fixedStep (in): User-supplied fixed time step (sec)
* \return Returns routing time step (sec)
*/
double dynwave_getRoutingStep(double fixedStep) {
    // --- use user-supplied fixed step if variable step option turned off
    //     or if its smaller than the min. allowable variable time step
    if (CourantFactor == 0.0)
        return fixedStep;
    if (fixedStep < MINTIMESTEP)
        return fixedStep;

    // --- at start of simulation (when current variable step is zero)
    //     use the minimum allowable time step
    if (VariableStep == 0.0) {
        VariableStep = MinRouteStep;
    }

    // --- otherwise compute variable step based on current flow solution
    else
        VariableStep = getVariableStep(fixedStep);

    // --- adjust step to be a multiple of a millisecond
    VariableStep = floor(1000.0 * VariableStep) / 1000.0;
    return VariableStep;
}

/*!
* \brief Routes flows through drainage network over current time step.
* @par Implementation arguments
 * - tStep (in): Time step (sec)
*/
int dynwave_execute(double tStep) {
    int converged;

    // --- initialize
    if (ErrorCode)
        return 0;
    Steps = 0;
    converged = FALSE;
    Omega = OMEGA;
    initRoutingStep();

    // --- keep iterating until convergence
    while (Steps < MaxTrials) {
        // --- execute a routing step & check for nodal convergence
        initNodeStates();
        findLinkFlows(tStep);
        if (HasConduitLosses)
            limitConduitLosses(tStep);
        converged = findNodeDepths(tStep);
        Steps++;
        if (Steps > 1) {
            if (converged)
                break;

            // --- check if link calculations can be skipped in next step
            findBypassedLinks();
        }
    }
    if (!converged)
        updateConvergenceStats();

    //  --- identify any capacity-limited conduits
    findLimitedLinks();
    return Steps;
}

//=============================================================================

/**
 * @brief Record a failed routing convergence event for non-outfall nodes.
 * @post Increments NonConvergeCount and each unconverged non-outfall node's
 * statistics. Outfalls are skipped because their convergence flags are not tested.
 * @pre Xnode convergence flags and node statistics refer to the current routing step.
 */
void updateConvergenceStats() {
    int i;
    NonConvergeCount++;
    for (i = 0; i < Nobjects[NODE]; i++) {
        // --- skip outfalls: findNodeDepths never tests them for convergence
        //     (their flag stays FALSE so outfall-connected links are not
        //     bypassed), so counting them here wrongly ranked boundary nodes
        //     under "Most Frequent Nonconverging Nodes". Diagnostic-only
        //     change: hydraulic convergence & bypass logic are unaffected.
        if (Node[i].type == OUTFALL)
            continue;
        stats_updateConvergenceStats(i, Xnode[i].converged);
    }
}

//=============================================================================

/**
 * @brief Reset iterative routing flags and preserve previous conduit areas.
 * @post Clears node convergence/dYdT and link bypass/surface-area fields;
 * copies Conduit.a1 into a2 as the prior-step area.
 * @pre Hydraulic node/link and Xnode arrays are allocated for the current model.
 */
void initRoutingStep() {
    int i;
    for (i = 0; i < Nobjects[NODE]; i++) {
        Xnode[i].converged = FALSE;
        Xnode[i].dYdT = 0.0;
    }
    HasConduitLosses = Evap.rate > 0.0;
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (Link[i].type == CONDUIT && Link[i].seepRate > 0.0)
            HasConduitLosses = TRUE;
        Link[i].bypassed = FALSE;
        Link[i].surfArea1 = 0.0;
        Link[i].surfArea2 = 0.0;
    }

    // --- a2 preserves conduit area from solution at last time step
    for (i = 0; i < Nlinks[CONDUIT]; i++)
        Conduit[i].a2 = Conduit[i].a1;
}

//=============================================================================

/**
 * @brief Initializes node's surface area, inflow & outflow.
 */
void initNodeStates() {
    int i;

    for (i = 0; i < Nobjects[NODE]; i++) {
        // --- initialize nodal surface area
        if (AllowPonding) {
            Xnode[i].newSurfArea = node_getPondedArea(i, Node[i].newDepth);
        } else {
            Xnode[i].newSurfArea = node_getSurfArea(i, Node[i].newDepth);
        }

        // --- initialize nodal inflow & outflow
        Node[i].inflow = 0.0;
        Node[i].outflow = Node[i].losses;
        if (Node[i].newLatFlow >= 0.0) {
            Node[i].inflow += Node[i].newLatFlow;
        } else {
            Node[i].outflow -= Node[i].newLatFlow;
        }
        Xnode[i].sumdqdh = 0.0;
        Xnode[i].conduitLoss = 0.0;
        Xnode[i].conduitVolumeChange = 0.0;
    }
}

//=============================================================================

/**
 * @brief Mark links bypassable when both endpoint nodes have converged.
 * @post Rewrites every Link.bypassed from the current endpoint Xnode.converged flags.
 * @pre All link endpoint indices and Xnode state are valid.
 */
void findBypassedLinks() {
    int i;
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (Xnode[Link[i].node1].converged && Xnode[Link[i].node2].converged)
            Link[i].bypassed = TRUE;
        else
            Link[i].bypassed = FALSE;
    }
}

//=============================================================================

/**
 * @brief Determines if a conduit link is capacity limited.
 */
void findLimitedLinks() {
    int j, n1, n2, k;
    double h1, h2;

    for (j = 0; j < Nobjects[LINK]; j++) {
        // ---- check only non-dummy conduit links
        if (!isTrueConduit(j))
            continue;

        // --- check that upstream end is full
        k = Link[j].subIndex;
        Conduit[k].capacityLimited = FALSE;
        if (Conduit[k].a1 >= Link[j].xsect.aFull) {
            // --- check if HGL slope > conduit slope
            n1 = Link[j].node1;
            n2 = Link[j].node2;
            h1 = Node[n1].newDepth + Node[n1].invertElev;
            h2 = Node[n2].newDepth + Node[n2].invertElev;
            if ((h1 - h2) > fabs(Conduit[k].slope) * Conduit[k].length)
                Conduit[k].capacityLimited = TRUE;
        }
    }
}

//=============================================================================

/**
 * @brief Compute current link flows and accumulate their endpoint contributions.
 * @param[in] dt Positive routing step in seconds.
 * @details Evaluates nonbypassed true conduits in an OpenMP loop using the
 * configured NumThreads, then updates node totals serially. Dummy conduits,
 * pumps and regulators are computed/accumulated afterward.
 * @pre Iteration Steps/Omega, bypass flags and node accumulators were initialized.
 * @post Updates link hydraulic states and endpoint node inflow/outflow,
 * surface-area and flow-head derivative totals.
 */
void findLinkFlows(double dt) {
    int i;
    // --- find new flow in each non-dummy conduit
#pragma omp parallel num_threads(NumThreads)
    {
#pragma omp for
        for (i = 0; i < Nobjects[LINK]; i++) {
            if (isTrueConduit(i) && !Link[i].bypassed)
                dwflow_findConduitFlow(i, Steps, Omega, dt);
        }
    }

    // --- update inflow/outflows for nodes attached to non-dummy conduits
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (isTrueConduit(i))
            updateNodeFlows(i);
    }

    // --- find new flows for all dummy conduits, pumps & regulators
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (!isTrueConduit(i)) {
            if (!Link[i].bypassed)
                findNonConduitFlow(i, dt);
            updateNodeFlows(i);
        }
    }
}

//=============================================================================

/**
 * @brief Distinguish physical conduits from dummy links and regulators.
 * @param[in] j Valid link index.
 * @return Nonzero only when Link[j].type is CONDUIT and its cross section is not DUMMY.
 */
int isTrueConduit(int j) {
    return (Link[j].type == CONDUIT && Link[j].xsect.type != DUMMY);
}

//=============================================================================

/**
 * @brief Finds new flow in a non-conduit-type link.
 *
 * @param[in] i Link index.
 * @param[in] dt Time step (sec).
 */
void findNonConduitFlow(int i, double dt) {
    double qLast; // previous link flow (cfs)
    double qNew;  // new link flow (cfs)

    // --- get link flow from last iteration
    qLast = Link[i].newFlow;
    Link[i].dqdh = 0.0;

    // --- get new inflow to link from its upstream node
    //     (link_getInflow returns 0 if flap gate closed or pump is offline)
    qNew = link_getInflow(i);
    if (Link[i].type == PUMP)
        qNew = getModPumpFlow(i, qNew, dt);

    // --- find surface area at each end of link
    findNonConduitSurfArea(i);

    // --- apply under-relaxation with flow from previous iteration;
    // --- do not allow flow to change direction without first being 0
    if (Steps > 0 && Link[i].type != PUMP) {
        qNew = (1.0 - Omega) * qLast + Omega * qNew;
        if (qNew * qLast < 0.0)
            qNew = 0.001 * SGN(qNew);
    }
    Link[i].newFlow = qNew;
}

//=============================================================================

/**
 * @brief Modifies pump curve pumping rate depending on amount of water available at pump's inlet
 *        node.
 *
 * @param[in] i Link index.
 * @param[in] q Pump flow from pump curve (cfs).
 * @param[in] dt Time step (sec).
 * @return Modified pump flow rate (cfs).
 */
double getModPumpFlow(int i, double q, double dt) {
    int j = Link[i].node1;    // pump's inlet node index
    int k = Link[i].subIndex; // pump's index
    double newNetInflow;      // inflow - outflow rate (cfs)
    double netFlowVolume;     // inflow - outflow volume (ft3)
    double y;                 // node depth (ft)

    if (q == 0.0)
        return q;

    // --- case where inlet node is a storage node:
    //     prevent node volume from going negative
    if ((Node[j].type == STORAGE || Node[j].storageArea > 0.0))
        return node_getMaxOutflow(j, q, dt);

    // --- case where inlet is a non-storage node
    switch (Pump[k].type) {
    // --- for Type1 pump, a volume is computed for inlet node,
    //     so make sure it doesn't go negative
    case TYPE1_PUMP:
        return node_getMaxOutflow(j, q, dt);

    // --- for other types of pumps, if pumping rate would make depth
    //     at upstream node negative, then set pumping rate = inflow
    case TYPE2_PUMP:
    case TYPE4_PUMP:
    case TYPE3_PUMP:
        newNetInflow = Node[j].inflow - Node[j].outflow - q;
        netFlowVolume = 0.5 * (Node[j].oldNetInflow + newNetInflow) * dt;
        y = Node[j].oldDepth + netFlowVolume / Xnode[j].newSurfArea;
        if (y <= 0.0)
            return Node[j].inflow;
    }
    return q;
}

//=============================================================================

/**
 * @brief Finds the surface area contributed by a non-conduit link to its upstream and downstream
 *        nodes.
 *
 * @param[in] i Link index.
 */
void findNonConduitSurfArea(int i) {
    if (Link[i].type == ORIFICE) {
        Link[i].surfArea1 = Orifice[Link[i].subIndex].surfArea / 2.;
    }

    // --- no surface area for weirs to maintain SWMM 4 compatibility
    else
        Link[i].surfArea1 = 0.0;

    Link[i].surfArea2 = Link[i].surfArea1;
    if (Link[i].flowClass == UP_CRITICAL || (Node[Link[i].node1].type == STORAGE || Node[Link[i].node1].storageArea > 0.0))
        Link[i].surfArea1 = 0.0;
    if (Link[i].flowClass == DN_CRITICAL || (Node[Link[i].node2].type == STORAGE || Node[Link[i].node2].storageArea > 0.0))
        Link[i].surfArea2 = 0.0;
}

//=============================================================================

/**
 * @brief Accumulate a link's current flows, losses and surface areas at its nodes.
 * @param[in] i Valid link index; reads signed Link[i].newFlow in cfs.
 * @post Updates Node endpoint inflow/outflow totals and Xnode surface-area
 * and sumdqdh totals. Conduit barrel counts and evaporation/seepage losses
 * are included; outfall nodes do not share those losses.
 */
void updateNodeFlows(int i)
//
//  Input:   i = link index
//           q = link flow rate (cfs)
//  Output:  none
//  Purpose: updates cumulative inflow & outflow at link's end nodes.
//
{
    int k;
    int barrels = 1;
    int n1 = Link[i].node1;
    int n2 = Link[i].node2;
    double q = Link[i].newFlow;
    double conduitLossRate = 0.0;

    // --- update total inflow & outflow at upstream/downstream nodes
    if (q >= 0.0) {
        Node[n1].outflow += q;
        Node[n2].inflow += q;
    } else {
        Node[n1].inflow -= q;
        Node[n2].outflow -= q;
    }

    // --- add any uniform evap & seepage loss from conduit link
    if (Link[i].type == CONDUIT) {
        k = Link[i].subIndex;
        barrels = Conduit[k].barrels;
        conduitLossRate = (Conduit[k].evapLossRate + Conduit[k].seepLossRate) * barrels;
        if (conduitLossRate > 0.0) {
            // --- outfall nodes do not share evap & seepage losses
            if (Node[n1].type != OUTFALL && Node[n2].type != OUTFALL)
                conduitLossRate /= 2.0;
            if (Node[n1].type != OUTFALL)
                Node[n1].outflow += conduitLossRate;
            if (Node[n2].type != OUTFALL)
                Node[n2].outflow += conduitLossRate;
        }
    }

    // Midpoint conduit flows exclude the change in the pipe's own inventory.
    // Physical chambers book their share explicitly, instead of treating a
    // crown-cutoff top width as additional real storage (issue #10 / USEPA #200).
    if (isTrueConduit(i)) {
        double delta = Link[i].newVolume - Link[i].oldVolume;
        double share = (Node[n1].type != OUTFALL && Node[n2].type != OUTFALL) ? 0.5 : 1.0;
        if (Node[n1].type != OUTFALL)
            Xnode[n1].conduitVolumeChange += share * delta;
        if (Node[n2].type != OUTFALL)
            Xnode[n2].conduitVolumeChange += share * delta;
    }

    // --- add surf. area contributions to upstream/downstream nodes
    Xnode[Link[i].node1].newSurfArea += Link[i].surfArea1 * barrels;
    Xnode[Link[i].node2].newSurfArea += Link[i].surfArea2 * barrels;

    // --- update summed value of dqdh at each end node
    Xnode[Link[i].node1].sumdqdh += Link[i].dqdh;
    if (Link[i].type == PUMP) {
        k = Link[i].subIndex;
        if (Pump[k].type != TYPE4_PUMP) {
            Xnode[n2].sumdqdh += Link[i].dqdh;
        }
    } else
        Xnode[n2].sumdqdh += Link[i].dqdh;
}

//=============================================================================

/**
 * @brief Limit conduit losses to the water their end nodes can supply.
 *
 * A conduit volume cap alone does not protect a dry end node: its share of
 * seepage can exceed the inflow while the other end holds water. Aggregate
 * all incident losses before sharing the available water, then use the more
 * restrictive end's factor. Keep the accepted rates in the conduit so water
 * quality and the routing ledger charge exactly the hydraulic loss.
 */
void limitConduitLosses(double dt) {
    int i, j, k, n1, n2;
    double rate, share, area, storageRate, available, factor, reduction;
    int hasLoss = FALSE;
    for (i = 0; i < Nobjects[NODE]; i++)
        Xnode[i].conduitLoss = 0.0;
    for (j = 0; j < Nobjects[LINK]; j++) {
        if (Link[j].type != CONDUIT)
            continue;
        k = Link[j].subIndex;
        rate = (Conduit[k].evapLossRate + Conduit[k].seepLossRate) * Conduit[k].barrels;
        if (rate <= 0.0)
            continue;
        hasLoss = TRUE;
        n1 = Link[j].node1;
        n2 = Link[j].node2;
        share = (Node[n1].type != OUTFALL && Node[n2].type != OUTFALL) ? 0.5 : 1.0;
        if (Node[n1].type != OUTFALL)
            Xnode[n1].conduitLoss += share * rate;
        if (Node[n2].type != OUTFALL)
            Xnode[n2].conduitLoss += share * rate;
    }
    if (!hasLoss)
        return;
    for (i = 0; i < Nobjects[NODE]; i++) {
        Xnode[i].lossFactor = 1.0;
        rate = Xnode[i].conduitLoss;
        if (rate <= 0.0 || Node[i].type == OUTFALL)
            continue;
        area = Node[i].storageArea > 0.0 ? Xnode[i].newSurfArea : MAX(Xnode[i].newSurfArea, MinSurfArea);
        // FUDGE is a numerical wetting floor, not a renewable water supply.
        // Enforce both the step's water budget and trapezoidal continuity.
        available = Node[i].inflow - Node[i].outflow + rate;
        storageRate = Node[i].storageArea > 0.0
                          ? (Node[i].oldVolume - Xnode[i].conduitVolumeChange) / dt
                          : MAX(0.0, Node[i].oldDepth - FUDGE) * area / dt;
        available = MIN(available + storageRate,
                        available + Node[i].oldNetInflow + 2.0 * storageRate);
        Xnode[i].lossFactor = MIN(1.0, MAX(0.0, available / rate));
    }
    for (j = 0; j < Nobjects[LINK]; j++) {
        if (Link[j].type != CONDUIT)
            continue;
        k = Link[j].subIndex;
        n1 = Link[j].node1;
        n2 = Link[j].node2;
        factor = MIN(Xnode[n1].lossFactor, Xnode[n2].lossFactor);
        if (factor >= 1.0)
            continue;
        rate = (Conduit[k].evapLossRate + Conduit[k].seepLossRate) * Conduit[k].barrels;
        reduction = rate * (1.0 - factor);
        Conduit[k].evapLossRate *= factor;
        Conduit[k].seepLossRate *= factor;
        share = (Node[n1].type != OUTFALL && Node[n2].type != OUTFALL) ? 0.5 : 1.0;
        if (Node[n1].type != OUTFALL)
            Node[n1].outflow -= share * reduction;
        if (Node[n2].type != OUTFALL)
            Node[n2].outflow -= share * reduction;
    }
}
//=============================================================================

/**
 * @brief Finds new depth at all nodes and checks if convergence achieved.
 *
 * @param[in] dt Time step (sec).
 * @return TRUE if depth change at all non-Outfall nodes is within the convergence tolerance and
 *         FALSE otherwise.
 */
int findNodeDepths(double dt) {
    int i;
    double yOld = 0.0; // previous node depth (ft)

    // --- compute outfall depths based on flow in connecting link
    for (i = 0; i < Nobjects[LINK]; i++)
        link_setOutfallDepth(i);

    // --- compute new depth for all non-outfall nodes and determine if
    //     depth change from previous iteration is below tolerance
    //
#pragma omp parallel num_threads(NumThreads)
    {
#pragma omp for private(yOld)
        for (i = 0; i < Nobjects[NODE]; i++) {
            if (Node[i].type == OUTFALL)
                continue;
            yOld = Node[i].newDepth;
            setNodeDepth(i, dt);
            Xnode[i].converged = TRUE;
            if (fabs(yOld - Node[i].newDepth) > HeadTol) {
                Xnode[i].converged = FALSE;
            }
        }
    }

    // --- return FALSE if any non-Outfall node failed to converge
    for (i = 0; i < Nobjects[NODE]; i++) {
        if (Node[i].type == OUTFALL)
            continue;
        if (Xnode[i].converged == FALSE)
            return FALSE;
    }
    return TRUE;
}

//=============================================================================

/**
 * @brief Sets depth at non-outfall node after current time step.
 *
 * @param[in] i Node index.
 * @param[in] dt Time step (sec).
 */
void setNodeDepth(int i, double dt) {
    int canPond;              // TRUE if node can pond overflows
    int isPonded;             // TRUE if node is currently ponded
    int isSurcharged = FALSE; // TRUE if node is surcharged
    double dQ;                // inflow minus outflow at node (cfs)
    double dV;                // change in node volume (ft3)
    double dy;                // change in node depth (ft)
    double yMax;              // max. depth at node (ft)
    double yOld;              // node depth at previous time step (ft)
    double yLast;             // previous node depth (ft)
    double yNew;              // new node depth (ft)
    double yCrown;            // depth to node crown (ft)
    double surfArea;          // node surface area (ft2)
    double denom;             // denominator term
    double corr;              // correction factor
    double f;                 // relative surcharge depth

    // --- see if node can pond water above it
    canPond = (AllowPonding && Node[i].pondedArea > 0.0);
    isPonded = (canPond && Node[i].newDepth > Node[i].fullDepth);

    // --- initialize value
    yCrown = Node[i].crownElev - Node[i].invertElev;
    yOld = Node[i].oldDepth;
    yLast = Node[i].newDepth;
    Node[i].overflow = 0.0;
    surfArea = Xnode[i].newSurfArea;
    if (Node[i].storageArea == 0.0)
        surfArea = MAX(surfArea, MinSurfArea);

    // --- determine average net flow volume into node over the time step
    dQ = Node[i].inflow - Node[i].outflow;
    dV = 0.5 * (Node[i].oldNetInflow + dQ) * dt;

    if (Node[i].storageArea > 0.0) {
        // Solve the actual chamber + half-pipe volume residual. The surface
        // area is only an iteration Jacobian; it never creates chamber water.
        double target = Node[i].oldVolume + dV - Xnode[i].conduitVolumeChange;
        double current = node_getVolume(i, yLast);
        double capacity = Node[i].fullVolume;
        if (canPond && yLast > Node[i].fullDepth)
            current += (yLast - Node[i].fullDepth) * Node[i].pondedArea;
        denom = surfArea + 0.5 * dt * Xnode[i].sumdqdh;
        if (Node[i].degree == 0 || denom <= 0.0) {
            yNew = MAX(0.0, target) / Node[i].storageArea;
            if (canPond && target > capacity)
                yNew = Node[i].fullDepth + (target - capacity) / Node[i].pondedArea;
        } else {
            yNew = yLast + (target - current) / denom;
            if (Steps > 0)
                yNew = (1.0 - Omega) * yLast + Omega * yNew;
        }
        yNew = MAX(0.0, yNew);
        yMax = Node[i].fullDepth + (canPond ? 0.0 : Node[i].surDepth);
        if (!canPond && yNew >= yMax) {
            yNew = yMax;
            Node[i].overflow = MAX(0.0, target - capacity) / dt;
        }
        Node[i].newVolume = node_getVolume(i, yNew);
        if (canPond && yNew > Node[i].fullDepth) {
            Node[i].newVolume += (yNew - Node[i].fullDepth) * Node[i].pondedArea;
            Node[i].overflow = MAX(0.0, Node[i].newVolume - MAX(Node[i].oldVolume, capacity)) / dt;
        }
        Node[i].newDepth = yNew;
        Xnode[i].oldSurfArea = surfArea;
        Xnode[i].dYdT = fabs(yNew - yOld) / dt;
        return;
    }

    // --- determine if node is EXTRAN surcharged
    if (SurchargeMethod == EXTRAN) {
        // --- ponded nodes don't surcharge
        if (isPonded)
            isSurcharged = FALSE;

        // --- closed storage units that are full are in surcharge
        else if (Node[i].type == STORAGE || Node[i].storageArea > 0.0) {
            isSurcharged = (Node[i].surDepth > 0.0 && yLast > Node[i].fullDepth);
        }

        // --- surcharge occurs when node depth exceeds top of its highest link
        else
            isSurcharged = (yCrown > 0.0 && yLast > yCrown);
    }

    // --- if node not surcharged, base depth change on surface area
    // or storage node is surcharged but there is not a flow change with response to head
    // An example is a storage node that has external inflows and outflows
    // but not connecting links.
    if (!isSurcharged || ((Node[i].type == STORAGE || Node[i].storageArea > 0.0) && Xnode[i].sumdqdh == 0.0)) {
        dy = dV / surfArea;
        yNew = yOld + dy;

        // --- save non-ponded surface area for use in surcharge algorithm
        if (!isPonded)
            Xnode[i].oldSurfArea = surfArea;

        // --- apply under-relaxation to new depth estimate
        if (Steps > 0) {
            yNew = (1.0 - Omega) * yLast + Omega * yNew;
        }

        // --- don't allow a ponded node to drop much below full depth
        if (isPonded && yNew < Node[i].fullDepth)
            yNew = Node[i].fullDepth - FUDGE;
    }
    // --- if node surcharged, base depth change on dqdh
    //     NOTE: depth change is w.r.t depth from previous
    //     iteration; also, do not apply under-relaxation.
    else {
        // --- apply correction factor for upstream terminal nodes
        corr = 1.0;
        if (Node[i].degree < 0)
            corr = 0.6;

        // --- allow surface area from last non-surcharged condition
        //     to influence dqdh if depth close to crown depth
        denom = Xnode[i].sumdqdh;
        if (yLast < 1.25 * yCrown) {
            f = (yLast - yCrown) / yCrown;
            denom += (Xnode[i].oldSurfArea / dt - Xnode[i].sumdqdh) * exp(-15.0 * f);
        }

        // --- compute new estimate of node depth
        if (denom == 0.0)
            dy = 0.0;
        else
            dy = corr * dQ / denom;
        yNew = yLast + dy;
        if (yNew < yCrown)
            yNew = yCrown - FUDGE;

        // --- don't allow a newly ponded node to rise much above full depth
        if (canPond && yNew > Node[i].fullDepth)
            yNew = Node[i].fullDepth + FUDGE;
    }

    // --- depth cannot be negative
    if (yNew < 0)
        yNew = 0.0;

    // --- determine max. non-flooded depth
    yMax = Node[i].fullDepth;
    if (canPond == FALSE)
        yMax += Node[i].surDepth;

    // --- find flooded depth & volume
    if (yNew > yMax) {
        yNew = getFloodedDepth(i, canPond, dV, yNew, yMax, dt);
    } else
        Node[i].newVolume = node_getVolume(i, yNew);

    // --- compute change in depth w.r.t. time
    Xnode[i].dYdT = fabs(yNew - yOld) / dt;

    // --- save new depth for node
    Node[i].newDepth = yNew;

    // --- A3 parity term tracing for one node (SWMM_TRACE_NODE=<index>,
    //     first 64 invocations; requires SWMM_TRACE_RSTEP for the path)
    {
        static FILE* nf = NULL;
        static long nfTarget = -2;
        static long nfSkip = 0;
        static int nfCount = 0;
        if (nfTarget == -2) {
            char* p = getenv("SWMM_TRACE_NODE");
            char* tr = getenv("SWMM_TRACE_RSTEP");
            char* sk = getenv("SWMM_TRACE_SKIP");
            nfTarget = -1;
            if (sk && *sk)
                nfSkip = atol(sk);
            if (p && *p && tr && *tr) {
                char fname[512];
                nfTarget = atol(p);
                snprintf(fname, sizeof(fname), "%s.node%ld", tr, nfTarget);
                nf = fopen(fname, "w");
                if (nf)
                    fprintf(nf, "n,yOld,yLast,dQ,dV,surfArea,sumdqdh,surch,yNew\n");
            }
        }
        if (nf && i == nfTarget) {
            ++nfCount;
            if (nfCount > nfSkip && nfCount <= nfSkip + 128) {
                fprintf(nf, "%d,%a,%a,%a,%a,%a,%a,%d,%a\n", nfCount, yOld, yLast, dQ, dV, surfArea,
                        Xnode[i].sumdqdh, isSurcharged, yNew);
                if (nfCount >= nfSkip + 128) {
                    fclose(nf);
                    nf = NULL;
                }
            }
        }
    }
}

//=============================================================================

/**
 * @brief Update volume and overflow for a node whose tentative depth exceeds its limit.
 * @param[in] i Valid node index into Node.
 * @param[in] canPond TRUE if excess volume may pond over the node, FALSE otherwise.
 * @param[in] dV Signed volume change over the step in cubic feet.
 * @param[in] yNew Tentative node depth in feet.
 * @param[in] yMax Maximum unponded node depth in feet.
 * @param[in] dt Positive routing step in seconds.
 * @return yMax without ponding, otherwise yNew, in feet.
 * @post Node[i].newVolume and overflow are updated. Overflow below FUDGE
 * is set to zero; without ponding the stored volume is clamped to fullVolume.
 */
double getFloodedDepth(int i, int canPond, double dV, double yNew, double yMax, double dt)
//
//  Input:   i  = node index
//           canPond = TRUE if water can pond over node
//           isPonded = TRUE if water is currently ponded
//           dV = change in volume over time step (ft3)
//           yNew = current depth at node (ft)
//           yMax = max. depth at node before ponding (ft)
//           dt = time step (sec)
//  Output:  returns depth at node when flooded (ft)
//  Purpose: computes depth, volume and overflow for a flooded node.
//
{
    if (canPond == FALSE) {
        Node[i].overflow = dV / dt;
        Node[i].newVolume = Node[i].fullVolume;
        yNew = yMax;
    } else {
        Node[i].newVolume = MAX((Node[i].oldVolume + dV), Node[i].fullVolume);
        Node[i].overflow = (Node[i].newVolume - MAX(Node[i].oldVolume, Node[i].fullVolume)) / dt;
    }
    if (Node[i].overflow < FUDGE)
        Node[i].overflow = 0.0;
    return yNew;
}

//=============================================================================

/**
 * @brief Finds time step that satisfies stability criterion but is no greater than the
 *        user-supplied max. time step.
 *
 * @param[in] maxStep User-supplied max. time step (sec).
 * @return Time step (sec).
 */
double getVariableStep(double maxStep) {
    int minLink = -1; // index of link w/ min. time step
    int minNode = -1; // index of node w/ min. time step
    double tMin;      // allowable time step (sec)
    double tMinLink;  // allowable time step for links (sec)
    double tMinNode;  // allowable time step for nodes (sec)

    // --- find stable time step for links & then nodes
    tMin = maxStep;
    tMinLink = getLinkStep(tMin, &minLink);
    tMinNode = getNodeStep(tMinLink, &minNode);

    // --- use smaller of the link and node time step
    tMin = tMinLink;
    if (tMinNode < tMin) {
        tMin = tMinNode;
        minLink = -1;
    }

    // --- update count of times the minimum node or link was critical
    stats_updateCriticalTimeCount(minNode, minLink);

    // --- don't let time step go below an absolute minimum
    if (tMin < MinRouteStep)
        tMin = MinRouteStep;
    return tMin;
}

//=============================================================================

/**
 * @brief Finds critical time step for conduits based on Courant criterion.
 *
 * @param[in] tMin Critical time step found so far (sec).
 * @param[out] minLink Index of link with critical time step;.
 * @return Critical time step (sec).
 */
double getLinkStep(double tMin, int* minLink) {
    int i;               // link index
    int k;               // conduit index
    double q;            // conduit flow (cfs)
    double t;            // time step (sec)
    double tLink = tMin; // critical link time step (sec)

    // --- examine each conduit link
    for (i = 0; i < Nobjects[LINK]; i++) {
        if (Link[i].type == CONDUIT) {
            // --- skip conduits with negligible flow, area or Fr
            k = Link[i].subIndex;
            q = fabs(Link[i].newFlow) / Conduit[k].barrels;
            if (q <= FUDGE || Conduit[k].a1 <= FUDGE || Link[i].froude <= 0.01)
                continue;

            // --- compute time step to satisfy Courant condition
            t = Link[i].newVolume / Conduit[k].barrels / q;
            t = t * Conduit[k].modLength / link_getLength(i);
            t = t * Link[i].froude / (1.0 + Link[i].froude) * CourantFactor;

            // --- update critical link time step
            if (t < tLink) {
                tLink = t;
                *minLink = i;
            }
        }
    }
    return tLink;
}

//=============================================================================

/**
 * @brief Finds critical time step for nodes based on max. allowable projected change in depth.
 *
 * @param[in] tMin Critical time step found so far (sec).
 * @param[out] minNode Index of node with critical time step;.
 * @return Critical time step (sec).
 */
double getNodeStep(double tMin, int* minNode) {
    int i;               // node index
    double maxDepth;     // max. depth allowed at node (ft)
    double dYdT;         // change in depth per unit time (ft/sec)
    double t1;           // time needed to reach depth limit (sec)
    double tNode = tMin; // critical node time step (sec)

    // --- find smallest time so that estimated change in nodal depth
    //     does not exceed safety factor * maxdepth
    for (i = 0; i < Nobjects[NODE]; i++) {
        // --- see if node can be skipped
        if (Node[i].type == OUTFALL)
            continue;
        if (Node[i].newDepth <= FUDGE)
            continue;
        if (Node[i].newDepth + FUDGE >= Node[i].crownElev - Node[i].invertElev)
            continue;

        // --- define max. allowable depth change using crown elevation
        maxDepth = (Node[i].crownElev - Node[i].invertElev) * 0.25;
        if (maxDepth < FUDGE)
            continue;
        dYdT = Xnode[i].dYdT;
        if (dYdT < FUDGE)
            continue;

        // --- compute time to reach max. depth & compare with critical time
        t1 = maxDepth / dYdT;
        if (t1 < tNode) {
            tNode = t1;
            *minNode = i;
        }
    }
    return tNode;
}
