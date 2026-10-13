// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SubsurfaceSolver.hpp"
#include "../../hydrology/RichardsColumn.hpp"

namespace openswmm::twoD {
// Internal water-only trial, not a runtime activation/receipt publication API.
// column uses SI relative-to-invert elevations. max_bottom_water supplies the
// hydraulic owner's receiving allowance (m3), including elastic pressure storage;
// subtracting initial bottom water bounds the external inflow award only. The
// owner must still validate the final state after forcing/internal redistribution.
struct RichardsBedBinding {
    int bed=-1;
    const richards::Column* column=nullptr;
    double invert=0, max_bottom_water=0, clogging=1;
};
struct RichardsBedReceipt {
    int node=-1, bed=-1, cell=-1;
    double lid_head=0, aquifer_head=0; // absolute SI sample heads, before trial
    double requested_down=0, requested_up=0, reserved_down=0, reserved_up=0;
    double accepted_down=0, accepted_up=0, unused_down=0, unused_up=0;
};
struct RichardsBedTrial {
    std::string error;
    // All arrays have stable ascending node order, independent of caller order.
    // Original columns and aquifer remain unchanged, including on success.
    std::vector<richards::Column> columns;
    std::vector<richards::Report> reports;
    std::vector<RichardsBedReceipt> receipts;
    std::vector<std::pair<int,SubsurfaceSolver::NodeWaterBudget>> budgets;
    int rhs=0, newton=0; // both proposal and bounded solves
};
// Freeze real cell budgets once, propose on private compatible vector batches,
// allocate directional awards jointly, then re-solve with ODE volume limits.
// No area-sum clamp, future-water borrowing, native-soil loss or generic-node
// debit. The caller must jointly validate/publish actual aquifer receipts and
// hydraulic trial state; current production mesh/checkpoint guards remain.
RichardsBedTrial trialRichardsBeds(const SubsurfaceSolver&, double interval,
                                 const std::vector<RichardsBedBinding>&);

// Internal acceptance primitive for an actual due GW owner firing, replacing
// that firing rather than advancing/pinning the owner at the LID cadence.
// max_water contains the hydraulic owner's final bounds for EVERY column cell
// (including surface and elastic storage). It must include accepted forcing.
struct RichardsBedOwner {
    int bed=-1;
    richards::Column* column=nullptr;
    double invert=0, clogging=1;
    std::vector<double> max_water;
};
struct RichardsBedAcceptance {
    std::string error;
    RichardsBedTrial trial;
    double balance=0; // combined column/GW interval residual, m3
};
// Serial owner boundary: freeze, trial, settle the real GW closure privately,
// validate the final hydraulic bounds and matching water, then publish once.
// Failures leave columns, aquifer, pending surface returns and clocks unchanged.
// Only water is supported. Generic mobile-node receipts are excluded for these
// beds. Routing/LID-state adoption, joint checkpoint and mesh activation follow.
RichardsBedAcceptance acceptRichardsBedsAtFiring(SubsurfaceSolver&, SurfaceStateData&,
    double start, double end, const std::vector<RichardsBedOwner>&);
}
