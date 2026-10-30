// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RichardsColumn.hpp
 * @brief     Richards-equation soil column state and numerical integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RICHARDS_COLUMN_HPP
#define OPENSWMM_RICHARDS_COLUMN_HPP
#include <string>
#include <vector>
#include <limits>

namespace openswmm::richards {
// All kernel quantities are SI. Storage includes explicitly authored elastic
// water storage; theta is pore water content and remains <= theta_s.
struct Material {
    double theta_r = 0.0, alpha = 0.0, n = 0.0, l = 0.5, specific_storage = 0.0;
};
struct Options {
    bool enabled = false;
    int cells_per_layer = 8;
    double atol = 1.e-7, rtol = 1.e-5, max_step = 30.0;
};
struct Cell {
    double volume = 0.0, area = 0.0, z = 0.0, dz = 0.0;
    // Surface theta_s is the open storage fraction; area is always the full
    // contact footprint. Porous theta_s is saturated pore water content.
    double theta_s = 1.0, Ks = 0.0, wilting = 0.0, potential_et = 0.0;
    Material material;
};
enum class Bottom { Sealed, FreeDrainage, Head };
struct Column {
    // First cell is a surface reservoir at z=soil-surface elevation; the
    // remaining cells are porous control volumes ordered top to bottom.
    std::vector<Cell> cells;
    std::vector<double> water;
    Options options;
    Bottom bottom = Bottom::Sealed;
    double bottom_K = 0.0, bottom_head = 0.0, bottom_distance = 0.0;
    // Internal, per-advance reservations in m3, separately in each direction.
    // No recycling of received water into a donor award within this interval.
    // Infinity preserves the existing standalone boundary. area=0 uses the
    // porous face geometry; Head connections may use a reviewed contact area.
    double bottom_out_limit = std::numeric_limits<double>::infinity();
    double bottom_in_limit = std::numeric_limits<double>::infinity();
    double bottom_area = 0.0, bottom_factor = 1.0;
    double inflow =
        0.0; // accepted, held surface supply, m3/s; zero preserves standalone preparation
};
// Negative indices: -2 surface supply, -3 evaporation, -4 bottom outflow, -5 bottom inflow.
struct Transfer {
    int from = -1, to = -1;
    double volume = 0.0;
};
struct Report {
    bool ok = true;
    std::string error;
    int accepted = 0, rejected = 0, rhs = 0, newton = 0;
    double min_step = 0.0, bottom_volume = 0.0, evaporation = 0.0, balance = 0.0;
    double bottom_outflow = 0.0, bottom_inflow = 0.0;
    double inflow = 0.0;                               // accepted integrated surface supply, m3
    int bottom_out_limited = 0, bottom_in_limited = 0; // accepted endpoints only
    std::vector<Transfer> transfers;
};
/** @brief Check the finite material ranges required by the column kernels.
 * @param p Material with residual fraction, inverse-metre alpha, n>1, nonnegative
 * pore-connectivity l and strictly positive specific storage in 1/m.
 * @param theta_s Saturated pore fraction in (0,1], strictly above theta_r>=0.
 * @return True exactly when all those scalar conditions hold. Does not inspect
 * conductivity, geometry, initial water or any Options value. */
bool valid(const Material& p, double theta_s);
/** @brief Check integration/discretization option ranges without considering enabled.
 * @param o cells_per_layer in [1,256], finite positive atol/rtol and max_step seconds.
 * @return True when these ranges hold, even when o.enabled is false. advance() does
 * not use enabled as a bypass or use cells_per_layer to construct the supplied cells. */
bool valid(const Options& o);
/** @brief Compute van Genuchten pore water content from pressure head.
 * @param p Valid material; alpha is 1/m and n dimensionless.
 * @param theta_s Valid saturated volumetric fraction above theta_r.
 * @param pressure Pressure head metres; >=0 returns theta_s, negative heads follow
 * the retention curve. Caller supplies a meaningful finite value or dry -infinity.
 * @return Dimensionless pore fraction; excludes elastic storage. Does not validate
 * material/range or clamp numerical failures. */
double theta(const Material& p, double theta_s, double pressure);
/** @brief Compute pore plus explicitly authored positive-head elastic storage.
 * @param p Valid material, specific_storage in 1/m.
 * @param theta_s Saturated volumetric pore fraction.
 * @param pressure Pressure head metres.
 * @return Dimensionless stored-water/volume ratio theta()+Ss*max(0,pressure); it may
 * exceed theta_s under positive pressure. No material/value validation occurs. */
double storage(const Material& p, double theta_s, double pressure);
/** @brief Invert volumetric storage to pressure head.
 * @param p Valid material including positive specific_storage.
 * @param theta_s Valid saturated pore fraction above residual fraction.
 * @param storage_fraction Total stored-water/volume ratio, including elastic storage.
 * @return Metres of pressure head: nonnegative elastic branch at/above theta_s,
 * negative retention-curve branch below, or negative infinity at/below theta_r.
 * No material/finite validation occurs; advance() requires porous water strictly
 * above the residual amount to avoid that singular lower boundary. */
double pressure(const Material& p, double theta_s, double storage_fraction);
/** @brief Evaluate van Genuchten-Mualem unsaturated hydraulic conductivity.
 * @param p Valid material with nonnegative pore-connectivity exponent l.
 * @param Ks Finite nonnegative saturated conductivity m/s.
 * @param pressure Pressure head metres; nonnegative heads return Ks.
 * @return Conductivity m/s, otherwise the relative-saturation reduction times Ks.
 * Does not validate inputs or supply error diagnostics for numerical-domain failures. */
double conductivity(const Material& p, double Ks, double pressure);

// Adaptive stiff BDF1 with step doubling. Compatible column sizes are packed
// cell-major, node-lane minor; each lane has its own clock and error test.
// Failed calls leave all supplied column states unchanged.
/** @brief Advance independently adaptive Richards columns with all-column rollback.
 * @param[in,out] columns Non-owning distinct nonnull pointers, each with at least two
 * cells: surface reservoir first, porous cells top to bottom. water holds m3 and
 * matches cell count. Validates positive finite volume/area/dz, finite z, theta_s in
 * (0,1], nonnegative Ks/wilting/ET, valid porous materials/options, and initial water
 * (nonnegative surface; strictly above residual volume in porous cells). Boundary
 * heads/distances/conductivities, areas/factors, reservations and supply must be
 * compatible finite/nonnegative values; reservation limits alone may be +infinity.
 * Counts/packed products must fit int. Pointer vector/geometry/options are retained.
 * @param dt Finite positive integration interval seconds; no external clock updated.
 * @return One report per input in original order, including accepted/rejected/Newton/
 * RHS counts, m3 transfers/balance/evaporation and separate bottom-direction receipts.
 * Invalid inputs mark all reports failed and leave columns unchanged. Compatible
 * sizes pack cell-major/lane-minor in groups up to 64; BDF1 step doubling with damped
 * Newton adapts each lane. Any solver failure rolls back every column, including
 * previous groups; returned reports may retain trial counts/transfers and must not
 * be booked when ok=false. Only successful water vectors publish; options.enabled
 * is not a gate. Empty input returns empty reports. Allocation exceptions propagate
 * before publication; caller serializes access and owns boundary reservations. */
std::vector<Report> advance(std::vector<Column*>& columns, double dt);
}
#endif
