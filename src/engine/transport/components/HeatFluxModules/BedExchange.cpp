// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      BedExchange.cpp
 * @brief     Plan H6b — implementation. See BedExchange.hpp for the derivation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "BedExchange.hpp"
#include "../../../data/ChannelThermalData.hpp"

#include <cmath>
#include <cstddef>

#include "HeatOverrides.hpp" // sedimentFor — PE2
#include "SurfaceExchange.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../data/TableData.hpp"
#include "../../../hydraulics/XSectBatch.hpp"
#include "../../../hydraulics/Link.hpp"

namespace openswmm::transport::heat {

namespace {

/**
 * @brief phi1(z) = expm1(z)/z, with the removable singularity taken.
 *
 * @details Evaluate the first exponential-integrator phi function with a small-argument
 * expansion.
 * @param[in] z Finite dimensionless exponential argument, normally nonpositive damping.
 * @return 1+z/2 for abs(z)<1e-8, otherwise expm1(z)/z. No overflow/domain repair; pure/no
 * allocation.
 */
double phi1(double z) noexcept {
    if (std::fabs(z) < 1e-8)
        return 1.0 + 0.5 * z; // series, exact to 1e-16
    return std::expm1(z) / z;
}

/**
 * @brief phi2(z) = (e^z - 1 - z)/z^2, with the removable singularity taken. Used only on the
 * near-degenerate branch, where the spectral formula's 1/(lambda1 - lambda2) is not usable.
 *
 * @details Evaluate the second exponential-integrator phi function with a small-argument
 * expansion.
 * @param[in] z Finite dimensionless exponential argument.
 * @return .5+z/6 for abs(z)<1e-5, otherwise (expm1(z)-z)/(z*z). Pure/no allocation; extreme
 * arithmetic can be nonfinite and caller checks accepted pair increments.
 */
double phi2(double z) noexcept {
    if (std::fabs(z) < 1e-5)
        return 0.5 + z / 6.0;
    return (std::expm1(z) - z) / (z * z);
}

/**
 * @brief Cross-section parameters for a link. The same shape `HeatFluxes.cpp` builds; kept local
 * rather than exported because widening that file's helper to a public seam is a bigger change
 * than repeating ten field copies, and CLAUDE.md §3 says touch only what must be touched.
 *
 * @details Copy prepared native conduit cross-section values into a geometry-query descriptor.
 * @param[in] links Stable SoA shape/dimensions/full-flow fields, native ft/ft2 conventions.
 * @param[in] uk Valid zero-based link index covered by every field, unchecked.
 * @return Owned translated XSectParams. No allocation, finite/shape validation or state
 * mutation.
 */
XSectParams buildXsp(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    const auto ls = links.xsect_shape[uk];
    xs.type = link::translateShape(ls);
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max = links.xsect_w_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max = links.xsect_s_max[uk];
    xs.y_bot = links.xsect_y_bot[uk];
    xs.a_bot = links.xsect_a_bot[uk];
    xs.s_bot = links.xsect_s_bot[uk];
    xs.r_bot = links.xsect_r_bot[uk];
    return xs;
}

} // namespace

// ---------------------------------------------------------------------------
// The coupled stepper
// ---------------------------------------------------------------------------
/**
 * @param[in] dt Positive finite accepted duration s; nonpositive/NaN returns zero increments.
 * @return Owned degC increments for water/bed, or both0 for nonviable capacities/nonpositive
 * duration or nonfinite computed increments. Does not signal refusal separately from legitimate
 * zero change.
 * @note g fields require finite nonnegative W/K conductances, positive J/K capacities and
 * qualified held deep-ground degC; viable() itself checks only capacity positivity. Uses
 * positive finite surface flux derivative only (otherwise held j0), area m2 may0 for closed
 * conduit. Isolated water/bed with no ground/surface forcing uses an energy-paired exponential
 * difference; general2x2 held linear system uses phi functions/repeated-eigenvalue expansion. No
 * canonical state mutation, allocation, clock advance or global rollback; caller adopts both
 * accepted increments and owns physical ledger.
 */
PairStep relaxPair(const BedCoupling& g, double t_w, double t_b, double j0, double j1, double h,
                   double area_m2, double dt) noexcept {
    PairStep out{};
    if (!g.viable() || !(dt > 0.0))
        return out;

    // Surface flux slope, W/m²/°C. `h <= 0` or a non-increasing flux means
    // there is no usable linearization; treat the surface term as a constant
    // forcing, which is exactly `relaxT`'s own fallback and keeps the two
    // functions agreeing about when the probe is trusted.
    double djdt = 0.0;
    if (h > 0.0) {
        const double s = (j1 - j0) / h;
        if (s > 0.0 && std::isfinite(s))
            djdt = s;
    }

    // The isolated two-body limit has a scalar closed form. Use it directly
    // when no external flux or deep-ground contact is active: the general
    // spectral matrix formula loses accuracy when hyporheic conductance is
    // many orders of magnitude larger than either body's capacity per step.
    // This limit is common for large, intentionally stiff exchange tests and
    // keeps both temperatures on the same conserved-energy trajectory.
    if (g.g_bg == 0.0 && djdt == 0.0 && j0 == 0.0) {
        const double total = g.c_w + g.c_b;
        const double rate = g.g_wb / g.c_w + g.g_wb / g.c_b;
        const double difference = (t_w - t_b) * std::expm1(-rate * dt);
        const double dw = (g.c_b / total) * difference;
        const double db = -(g.c_w / g.c_b) * dw;
        if (!std::isfinite(dw) || !std::isfinite(db))
            return out;
        out.dt_w = dw;
        out.dt_b = db;
        return out;
    }

    // du/dt = M u + c, u = (T_w - t_w, T_b - t_b), u(0) = 0.
    const double a11 = -(area_m2 * djdt + g.g_wb) / g.c_w;
    const double a12 = g.g_wb / g.c_w;
    const double a21 = g.g_wb / g.c_b;
    const double a22 = -(g.g_wb + g.g_bg) / g.c_b;

    const double c1 = (-area_m2 * j0 + g.g_wb * (t_b - t_w)) / g.c_w;
    const double c2 = (g.g_wb * (t_w - t_b) + g.g_bg * (g.t_gr - t_b)) / g.c_b;

    // Eigenvalues. The discriminant is (a11-a22)^2 + 4*a12*a21 with both
    // off-diagonals >= 0, so it is never negative: the pair relaxes, it never
    // oscillates, and there is no dt at which this overshoots.
    const double tr = a11 + a22;
    const double diff = a11 - a22;
    const double disc = diff * diff + 4.0 * a12 * a21;
    const double s = std::sqrt(disc > 0.0 ? disc : 0.0);

    // phi1(M*dt) = alpha*M + beta*I, by the spectral formula for a 2x2.
    double alpha = 0.0, beta = 0.0;
    if (s * dt > 1e-8) {
        const double l1 = 0.5 * (tr + s);
        const double l2 = 0.5 * (tr - s);
        const double p1 = phi1(l1 * dt);
        const double p2 = phi1(l2 * dt);
        alpha = (p1 - p2) / s;
        beta = (p2 * l1 - p1 * l2) / s;
    } else {
        // Near-degenerate: the spectral formula divides by s. The confluent
        // form is the limit and needs no division —
        //   phi1(M dt) = phi1(l dt) I + dt phi2(l dt) (M - l I),
        // and since phi1(z) = 1 + z phi2(z) identically, the identity
        // coefficient collapses to exactly 1.
        const double l = 0.5 * tr;
        alpha = dt * phi2(l * dt);
        beta = 1.0;
    }

    // u = dt * phi1(M dt) * c = dt * (alpha * M c + beta * c)
    const double mc1 = a11 * c1 + a12 * c2;
    const double mc2 = a21 * c1 + a22 * c2;
    const double dw = dt * (alpha * mc1 + beta * c1);
    const double db = dt * (alpha * mc2 + beta * c2);

    // A non-finite result means the linearization produced something the
    // caller must not absorb. Refusing to move is the safe answer and the
    // one `relaxT` also takes.
    if (!std::isfinite(dw) || !std::isfinite(db))
        return out;
    out.dt_w = dw;
    out.dt_b = db;
    return out;
}

/**
 * @details Both concentrations must use the same native units; no unit conversion or physical
 * inventory mutation occurs. Positive finite volumes m3, exchange m3/s and dt s are required;
 * each nonpositive/NaN argument returns zero. Exactly decays concentration difference and pairs
 * increments by volume shares; computed nonfinite share returns zero silently, not an error
 * diagnostic. No independent concentration positivity/finite validation or clock mutation;
 * pure/allocation-free.
 */
SolutePairStep exchangePair(double c_w, double c_b, double vol_w, double vol_b, double q_exch,
                            double dt) noexcept {
    SolutePairStep out{};
    if (!(vol_w > 0.0) || !(vol_b > 0.0) || !(q_exch > 0.0) || !(dt > 0.0))
        return out;

    // Two bodies exchanging with nothing else: total mass is invariant and
    // the DIFFERENCE decays exponentially. Solving in those coordinates
    // makes conservation STRUCTURAL — there is no separate ledger step that
    // could be forgotten. It is not bit-exact, because `vol_w*(vol_b*share)`
    // and `vol_b*(vol_w*share)` associate differently; the residual is
    // relative round-off and the gate asserts that bound. See the header.
    const double vt = vol_w + vol_b;
    const double mu = q_exch * vt / (vol_w * vol_b); // 1/s
    const double d0 = c_w - c_b;
    const double dd = d0 * std::expm1(-mu * dt); // d(t) - d0

    const double share = dd / vt;
    if (!std::isfinite(share))
        return out;
    out.dc_w = vol_b * share;
    out.dc_b = -vol_w * share;
    return out;
}

// ---------------------------------------------------------------------------
// Configuration and geometry
// ---------------------------------------------------------------------------
/**
 * @brief Test current native finite-bed heat family process enable.
 * @param[in] ctx Stable global process/family flags.
 * @return heat_transport&&sediment_exchange, independent of current wetness/bed vector sizing.
 * @note Read-only/no allocation; does not prepare, seed or qualify any physical owner.
 */
bool bedExchangeEnabled(const SimulationContext& ctx) noexcept {
    return ctx.options.heat_transport && ctx.heat_config.sediment_exchange;
}

/**
 * @brief Sample the global held deep-ground temperature at native absolute model time.
 * @param[in,out] ctx Stable sediment configuration/table store/current_date; lookup can advance
 * selected table's mutable cursor without changing table ordinates or engine clock.
 * @return degC from in-range configured ground series, else configured constant. Does not
 * require a positive series value or independently validate finite output.
 * @note noexcept dependency lookup must not throw; exclusive cursor access, no physical heat
 * advance.
 */
double groundTemperature(SimulationContext& ctx) noexcept {
    const auto& sc = ctx.heat_config.sediment;
    if (sc.ground_ts_index >= 0 && sc.ground_ts_index < static_cast<int>(ctx.tables.count()))
        return table_tseries_lookup_cursor(ctx.tables[sc.ground_ts_index], ctx.current_date);
    return sc.ground_temp;
}

/**
 * @brief Compute wetted conduit bed contact area from native geometry.
 * @param[in] ctx Stable valid conduit subtype mapping, native dimensions/depth/length/barrels.
 * @param[in] link Valid zero-based native link index, unchecked before several array accesses.
 * @return (wet cross-section area/hydraulic radius)*positive length*barrels converted ft2 to m2,
 * zero for non-conduit/nonpositive length/depth/area/radius/result. Positive original length
 * preferred, else mod_length. Closed conduit still has wet perimeter; no free-surface
 * requirement.
 * @note No general finite/shape validation, allocation or physical/clock mutation; valid
 * geometry query dependencies must be nonthrowing for noexcept contract.
 */
double linkBedAreaM2(const SimulationContext& ctx, int link) noexcept {
    const auto uj = static_cast<std::size_t>(link);
    const int cr = ctx.link_subtypes.conduit_row(link);
    if (cr < 0)
        return 0.0; // regulators have no bed
    const auto ucr = static_cast<std::size_t>(cr);
    const auto& CD = ctx.link_subtypes.conduits;

    double length = CD.length[ucr];
    if (!(length > 0.0))
        length = CD.mod_length[ucr];
    if (!(length > 0.0))
        return 0.0;

    const double depth = ctx.links.depth[uj];
    if (!(depth > 0.0))
        return 0.0;

    // Contact area = wetted perimeter x length x barrels. P = A/R, the
    // engine's own identity — NOT the reference's top width, which is zero
    // for a full pipe. See BedZoneData.hpp divergence 1.
    const auto xs = buildXsp(ctx.links, uj);
    const double area = xsect::getAofY(xs, depth);
    const double rhyd = xsect::getRofY(xs, depth);
    if (!(area > 0.0) || !(rhyd > 0.0))
        return 0.0;
    const double perim_ft = area / rhyd;

    const double m2 = perim_ft * length * static_cast<double>(CD.barrels[ucr]) * kSqFtToSqM;
    return (m2 > 0.0) ? m2 : 0.0;
}

/**
 * @brief Compute native open-conduit free surface for heat/evaporation binding.
 * @param[in] ctx Stable prepared subtype/shape/geometry, length and barrel metadata.
 * @param[in] link Valid zero-based native link index, unchecked.
 * @return Top width*length*barrels ft2; zero for non-conduit/closed/nonpositive
 * length/depth/width. Original positive length preferred, else mod_length. No array/finite
 * validation or allocation; read-only/nonthrowing geometry dependencies, no physical/clock
 * mutation.
 */
double linkFreeSurfaceFt2(const SimulationContext& ctx, int link) noexcept {
    const auto uj = static_cast<std::size_t>(link);
    const int cr = ctx.link_subtypes.conduit_row(link);
    if (cr < 0)
        return 0.0;
    if (!xsect::isOpen(ctx.links.xsect_batch_shape[uj]))
        return 0.0;
    const auto ucr = static_cast<std::size_t>(cr);
    const auto& CD = ctx.link_subtypes.conduits;

    double length = CD.length[ucr];
    if (!(length > 0.0))
        length = CD.mod_length[ucr];
    const double depth = ctx.links.depth[uj];
    if (!(length > 0.0) || !(depth > 0.0))
        return 0.0;
    const auto xs = buildXsp(ctx.links, uj);
    const double top_width = xsect::getWofY(xs, depth);
    if (!(top_width > 0.0))
        return 0.0;
    return top_width * length * static_cast<double>(CD.barrels[ucr]);
}

/**
 * @brief Compute total finite-bed solute exchange rate from diffusion and hyporheic velocity.
 * @param[in] cfg Qualified SI sediment config, diffusivity m2/s, thickness m and hyporheic
 * velocity m/s.
 * @param[in] bed_m2 Nonnegative contact area m2.
 * @return Ds*A/thickness+velocity*A m3/s, zero for nonpositive/NaN area or thickness. No
 * independent coefficient finite/sign validation; pure/no allocation/water-native-clock
 * mutation.
 */
double bedExchangeQ(const SedimentConfig& cfg, double bed_m2) noexcept {
    if (!(bed_m2 > 0.0) || !(cfg.bed_thickness > 0.0))
        return 0.0;
    return cfg.solute_diffusivity * bed_m2 / cfg.bed_thickness + cfg.hyporheic_velocity * bed_m2;
}

/**
 * @brief Choose one element's held deep-ground constant or globally sampled series temperature.
 * @param[in] ctx Stable global sediment.ground_ts_index configuration.
 * @param[in] sc Resolved per-element sediment block with ground_temp degC.
 * @param[in] t_gr_global Previously sampled global ground temperature degC.
 * @return Global sample if global index>=0 (even if out of range), else element constant. No
 * sampling, validation, allocation, owner or clock mutation; caller resolves global series once
 * per interval.
 */
double groundTempFor(const SimulationContext& ctx, const SedimentConfig& sc,
                     double t_gr_global) noexcept {
    // The TIMESERIES spelling is GLOBAL-only by grammar, so when one is
    // configured it IS the boundary everywhere; otherwise each element's
    // resolved constant is (per-element rows included, and identical to the
    // global on a model with none).
    return (ctx.heat_config.sediment.ground_ts_index >= 0) ? t_gr_global : sc.ground_temp;
}

/**
 * @brief Publish native conduit thermal region geometry in SI.
 * @param[in] ctx Stable native link/subtype/shape/length/depth/barrel metadata.
 * @param[in,out] region Its link selects valid conduit; receives length/depth m, free
 * surface/bed m2. Invalid link/non-conduit returns leaving all existing geometry unchanged.
 * @note Positive length preferred else mod_length; direct length/depth values are not clamped
 * here, area helpers apply their own positivity tests. No allocation/physics/clock change;
 * noexcept valid geometry queries and exclusive region output required.
 */
void fillChannelThermalGeometry(const SimulationContext& ctx,
                                ChannelThermalRegion& region) noexcept {
    const int link = region.link;
    if (link < 0 || link >= ctx.n_links())
        return;
    const int row = ctx.link_subtypes.conduit_row(link);
    if (row < 0)
        return;
    const auto& conduits = ctx.link_subtypes.conduits;
    const double length =
        conduits.length[row] > 0 ? conduits.length[row] : conduits.mod_length[row];
    region.length = length * .3048;
    region.depth = ctx.links.depth[link] * .3048;
    region.surface_area = linkFreeSurfaceFt2(ctx, link) * kSqFtToSqM;
    region.bed_area = linkBedAreaM2(ctx, link);
}

/**
 * @brief Construct finite water/bed conductance and capacity from one SI contact footprint.
 * @param[in] ctx Stable positive SI water density kg/m3 and specific heat J/(kg*K).
 * @param[in] sc Qualified SI thermal diffusivity m2/s, thickness/deep depth m, solid density
 * kg/m3, specific heat J/(kg*K), hyporheic velocity m/s and held ground parameters.
 * @param[in] bed_m2 Nonnegative contact area m2.
 * @param[in] vol_ft3 Positive native water volume ft3, converted once to m3 for heat capacity.
 * @param[in] t_gr Held selected deep-ground temperature degC.
 * @return Owned coupling with water/bed J/K capacities, W/K conductive+advective water/bed and
 * conductive bed/ground links, ground degC. Zero coupling for nonpositive/NaN
 * area/water/thickness. Bed capacity uses bulk footprint*thickness, not porosity. Nonpositive
 * deep depth makes ground link0.
 * @note No general finite/coefficient validation, allocation, state/clock mutation; caller
 * checks viability and physical profile before adopting relaxPair results.
 */
BedCoupling bedCouplingFromContact(const SimulationContext& ctx, const SedimentConfig& sc,
                                   double bed_m2, double vol_ft3, double t_gr) noexcept {
    BedCoupling g{};
    if (!(bed_m2 > 0.0) || !(vol_ft3 > 0.0))
        return g;

    const double rho_cp_s = sc.sed_density * sc.sed_specific_heat; // J/m3/K
    const double y_bed = sc.bed_thickness;
    const double y_gr = sc.ground_depth;
    if (!(y_bed > 0.0))
        return g;

    // Conduction: alpha * rho_s * c_s has units W/m/K, so dividing by a
    // length and multiplying by an area gives W/K. This is the reference's
    // grouping (`element.cpp:132`) rather than a conductivity k, because
    // alpha is the parameter the reference exposes and re-deriving k here
    // would silently invent a second material description.
    const double k_eff = sc.thermal_diffusivity * rho_cp_s; // W/m/K
    const double g_cond = k_eff * bed_m2 / y_bed;
    const double g_adv = ctx.options.water_density * ctx.options.water_specific_heat *
                         sc.hyporheic_velocity * bed_m2; // W/K

    g.g_wb = g_cond + g_adv;
    g.g_bg = (y_gr > 0.0) ? (k_eff * bed_m2 / y_gr) : 0.0;
    g.c_w = ctx.options.water_density * ctx.options.water_specific_heat * vol_ft3 * kCuFtToCuM;
    g.c_b = rho_cp_s * bed_m2 * y_bed;
    g.t_gr = t_gr;
    return g;
}

/**
 * @brief Build a conduit finite-bed pair from current wet perimeter and resolved element
 * material.
 * @param[in] ctx Stable prepared link/native geometry/global+resolved bed overrides/water
 * constants.
 * @param[in] link Valid zero-based link index.
 * @param[in] vol_ft3 Accepted positive native conduit water volume ft3.
 * @param[in] t_gr Already sampled global deep-ground degC, used when global series configured.
 * @return Constructed SI coupling, zero/nonviable for nonpositive water/no wet footprint/invalid
 * capacity. Uses element sedimentFor and groundTempFor, no weather refresh or physical advance.
 * @note Read-only/no allocation; caller retains stable material/geometry and validates profile.
 */
BedCoupling bedCouplingForLink(const SimulationContext& ctx, int link, double vol_ft3,
                               double t_gr) noexcept {
    if (!(vol_ft3 > 0.0))
        return BedCoupling{};
    // PE2: the link's own bed attributes. On a model with no overrides this
    // is the very global object the pre-PE code read.
    const SedimentConfig& sc = sedimentFor(ctx, HeatElement::link(link));
    return bedCouplingFromContact(ctx, sc, linkBedAreaM2(ctx, link), vol_ft3,
                                  groundTempFor(ctx, sc, t_gr));
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
/**
 * @brief Size and seed native link bed temperatures at their initial owner phase.
 * @param[in,out] ctx Retained bed state and current native link count/global sediment config.
 * @note Uses global explicit initial degC if configured, else global ground constant (not
 * current series/per-element override). Shape mismatch resets entire vector and seeded flag,
 * then seeds every entry; matching already-seeded owner retained unchanged. Does not filter
 * active/wet/conduit links. Allocation may throw; exclusive initial/rebuild lifecycle required,
 * no physical interval or engine clock advance. Calling after count change intentionally
 * replaces evolved bed temperatures.
 */
void seedBedTemperature(SimulationContext& ctx) {
    auto& bed = ctx.bed_state;
    const int nl = ctx.n_links();
    const auto& sc = ctx.heat_config.sediment;
    const double t0 = sc.has_initial_temp ? sc.initial_temp : sc.ground_temp;

    // Temperature and solute extents are sized INDEPENDENTLY. A single
    // `resize` would have to know the species count, and the heat binding
    // runs first and does not — so a combined resize would either wipe the
    // solute store on every heat step or force the heat step to guess a
    // dimension that belongs to the transport engine.
    if (!bed.sized(nl)) {
        bed.link_temp.assign(static_cast<std::size_t>(nl > 0 ? nl : 0), t0);
        bed.seeded = false;
    }
    if (!bed.seeded) {
        // The bed starts at the configured initial temperature, falling back
        // to the ground temperature — the only number in the configuration
        // that describes the subsurface. It deliberately does NOT start at
        // the water's INITIAL_STATE: a bed that starts at the channel
        // temperature exchanges nothing for the first steps and would hide a
        // mis-wired coupling behind a plausible flat line.
        for (auto& t : bed.link_temp)
            t = t0;
        bed.seeded = true;
    }
}

/**
 * @param[in,out] ctx Prepared native hydraulics/material overrides and finite bed state; may
 * seed bed temperature and resize/reset combined bed native concentration array for
 * n_links*n_total.
 * @param[in,out] link_conc Optional writable contiguous node-major-style link array
 * [link*n_arr+s] in native concentration units; null returns. Caller guarantees at least
 * n_links*n_arr elements, nonaliasing with bed storage (which may reallocate), stable through
 * call.
 * @param[in] dt Positive finite accepted solute interval s; nonpositive/NaN returns.
 * @note Requires enabled bed heat family, positive n_arr, nonnegative offset and
 * n_total>=offset+n_arr with arithmetic products representable. Header describes combined bed
 * row mapping. Shape mismatch zero-initializes all bed concentrations, not just selected slice.
 * Skips mesh-contact-owned links, dry/no-wet-bed/nonpositive-exchange/contact-volume; remaining
 * species use exact paired exchange with bulk bed_m2*thickness m3 and water ft3 converted to m3.
 * Does not move water, book a global native mass ledger or advance clock. Progressive
 * concentration changes/allocations can throw without rollback; exclusive matching link/bed
 * native owner and once-only accepted-step invocation required.
 */
void applyBedSoluteExchange(SimulationContext& ctx, double* link_conc, int n_arr, int offset,
                            int n_total, double dt) {
    if (!bedExchangeEnabled(ctx) || !(dt > 0.0) || link_conc == nullptr)
        return;
    if (n_arr <= 0 || offset < 0 || n_total < offset + n_arr)
        return;
    seedBedTemperature(ctx);

    auto& bed = ctx.bed_state;
    const int nl_all = ctx.n_links();
    const auto need =
        static_cast<std::size_t>(nl_all > 0 ? nl_all : 0) * static_cast<std::size_t>(n_total);
    if (bed.n_species != n_total || bed.link_conc.size() != need) {
        // First solute step, or the species count changed. Seeding the bed
        // at ZERO is the honest default: the reference has no initial-bed
        // concentration input either, and inventing one would put mass into
        // the system that no deck asked for.
        bed.link_conc.assign(need, 0.0);
        bed.n_species = n_total;
    }

    const int nl = ctx.n_links();
    const auto na = static_cast<std::size_t>(n_arr);
    const auto nt = static_cast<std::size_t>(n_total);
    const auto off = static_cast<std::size_t>(offset);

    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (uj < ctx.heat_state.mesh_contact_link.size() && ctx.heat_state.mesh_contact_link[uj])
            continue;
        if (uj >= bed.link_temp.size())
            break;

        const double vol_ft3 = ctx.links.volume[uj];
        if (!(vol_ft3 > 0.0))
            continue;
        const double bed_m2 = linkBedAreaM2(ctx, j);
        if (!(bed_m2 > 0.0))
            continue;

        // PE2: this link's bed material — thickness and exchange rates may
        // be overridden per element, and the solute store's volume must
        // agree with the heat pair's about the same bed.
        const SedimentConfig& sc = sedimentFor(ctx, HeatElement::link(j));
        const double vol_w = vol_ft3 * kCuFtToCuM;
        const double vol_b = bed_m2 * sc.bed_thickness;
        const double q_exch = bedExchangeQ(sc, bed_m2);
        if (!(q_exch > 0.0) || !(vol_b > 0.0))
            continue;

        for (std::size_t s = 0; s < na; ++s) {
            double& cw = link_conc[uj * na + s];
            double& cb = bed.link_conc[uj * nt + off + s];
            const SolutePairStep st = exchangePair(cw, cb, vol_w, vol_b, q_exch, dt);
            cw += st.dc_w;
            cb += st.dc_b;
        }
    }
}

} // namespace openswmm::transport::heat
